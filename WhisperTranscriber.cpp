#include "WhisperTranscriber.h"
#include <QDebug>
#include <whisper.h>

WhisperTranscriber::WhisperTranscriber(QObject *parent)
    : QObject(parent)
{
}

WhisperTranscriber::~WhisperTranscriber()
{
    // 必须先取消并 join 工作线程：否则 whisper_free 之后 whisper_full 仍在读取已释放的 ctx
    cancel();
    if (m_worker) {
        m_worker->wait();
        delete m_worker;
        m_worker = nullptr;
    }
    // 加载线程同理：它正在 whisper_init_from_file 时释放 ctx 会崩在 llama/whisper 内部
    if (m_loadThread) {
        m_loadThread->wait();
        delete m_loadThread;
        m_loadThread = nullptr;
    }
    QMutexLocker locker(&m_mutex);
    if (m_whisperCtx) {
        whisper_free(m_whisperCtx);
        m_whisperCtx = nullptr;
    }
}

bool WhisperTranscriber::abortCallback(void *userData)
{
    return static_cast<WhisperTranscriber*>(userData)->m_cancelled.load();
}

bool WhisperTranscriber::loadModel(const QString &modelPath)
{
    QMutexLocker locker(&m_mutex);
    if (m_whisperCtx) {
        whisper_free(m_whisperCtx);
        m_whisperCtx = nullptr;
    }

    m_whisperCtx = whisper_init_from_file(modelPath.toUtf8().constData());
    if (!m_whisperCtx) {
        QString err = QString("Whisper模型加载失败: %1").arg(modelPath);
        qWarning() << "[Whisper]" << err;
        emit modelLoaded(false, err);
        return false;
    }

    m_modelPath = modelPath;
    qDebug() << "[Whisper] 模型加载成功:" << modelPath;
    emit modelLoaded(true, QString());
    return true;
}

void WhisperTranscriber::loadModelAsync(const QString &modelPath)
{
    if (m_loadThread) {          // 上一个加载还没结束（通常不会发生）先等它
        m_loadThread->wait();
        delete m_loadThread;
        m_loadThread = nullptr;
    }
    m_loadThread = QThread::create([this, modelPath]() { loadModel(modelPath); });
    m_loadThread->start();
}

QVector<TranscriptSegment> WhisperTranscriber::transcribe(const QVector<float> &samples, const QString &language)
{
    QVector<TranscriptSegment> result;

    if (!m_whisperCtx) {
        emit errorOccurred("Whisper模型未加载");
        return result;
    }

    if (samples.isEmpty()) {
        emit errorOccurred("音频数据为空");
        return result;
    }

    QMutexLocker locker(&m_mutex);   // 与 loadModel / 析构互斥，避免 ctx 被并发释放
    qDebug() << "[Whisper] 准备识别，样本数:" << samples.size()
             << "时长:" << (float)samples.size() / 16000.0f << "秒";

    if (samples.size() > 16000 * 600) {  // 超过10分钟警告
        qWarning() << "[Whisper] 音频超过10分钟，可能需要较长时间";
    }

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_realtime = false;
    params.print_progress = false;
    params.print_timestamps = false;
    params.print_special = false;
    params.translate = false;  // 只识别不翻译，翻译由Index-Translate处理

    if (language != "auto") {
        // language是const char*指针，直接赋值字符串字面量
        if (language == "zh") params.language = "zh";
        else if (language == "en") params.language = "en";
        else if (language == "ja") params.language = "ja";
        else if (language == "ko") params.language = "ko";
        else if (language == "fr") params.language = "fr";
        else if (language == "de") params.language = "de";
        else if (language == "es") params.language = "es";
        else if (language == "ru") params.language = "ru";
        else params.language = "auto";
        qDebug() << "[Whisper] 指定语言:" << params.language;
    } else {
        params.language = nullptr;  // auto detect
        qDebug() << "[Whisper] 自动检测语言";
    }

    params.n_threads = qMax(1, qMin(8, QThread::idealThreadCount()));
    params.no_context = true;
    params.single_segment = false;
    // 让 whisper_full 能在 cancel()/析构时提前返回（原本只能整段跑完）
    params.abort_callback = &WhisperTranscriber::abortCallback;
    params.abort_callback_user_data = this;

    qDebug() << "[Whisper] 调用 whisper_full...";
    int ret = whisper_full(m_whisperCtx, params, samples.constData(), (int)samples.size());
    qDebug() << "[Whisper] whisper_full 返回:" << ret;
    if (ret != 0) {
        emit errorOccurred(QString("Whisper识别失败，错误码: %1").arg(ret));
        return result;
    }

    if (m_cancelled) {
        qDebug() << "[Whisper] 识别已取消";
        return result;
    }

    int nSegments = whisper_full_n_segments(m_whisperCtx);
    qDebug() << "[Whisper] 识别完成，段数:" << nSegments;

    for (int i = 0; i < nSegments; i++) {
        if (m_cancelled) break;

        TranscriptSegment seg;
        seg.start = whisper_full_get_segment_t0(m_whisperCtx, i) * 0.01;  // 10ms -> 秒
        seg.end = whisper_full_get_segment_t1(m_whisperCtx, i) * 0.01;
        const char *text = whisper_full_get_segment_text(m_whisperCtx, i);
        seg.text = QString::fromUtf8(text).trimmed();

        if (!seg.text.isEmpty()) {
            result.append(seg);
            qDebug() << "[Whisper] 段" << i << ":" << seg.start << "-" << seg.end << seg.text.left(50);
        }

        emit transcribeProgress((i + 1) * 100 / nSegments);
    }

    return result;
}

void WhisperTranscriber::transcribeAsync(const QVector<float> &samples, const QString &language)
{
    // 同一时刻只允许一个识别任务：先 join 上一个，避免两个 whisper_full 共用同一 ctx
    if (m_worker) {
        m_worker->wait();
        delete m_worker;
        m_worker = nullptr;
    }
    m_cancelled = false;

    m_worker = QThread::create([this, samples, language]() {
        QVector<TranscriptSegment> result = transcribe(samples, language);
        if (!m_cancelled) {
            emit transcribeFinished(result);
        }
    });
    m_worker->start();
}

void WhisperTranscriber::cancel()
{
    m_cancelled = true;
}
