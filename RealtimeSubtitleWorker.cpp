#include "RealtimeSubtitleWorker.h"

#include "SileroVad.h"
#include "Translator.h"
#include "WhisperTranscriber.h"

#include <QElapsedTimer>
#include <QTimer>

#include <algorithm>

namespace {
constexpr int kSampleRate = 16000;
constexpr int kFrameSamples = SileroVad::kWindowSamples;   // 512
constexpr int kWindowsPerDetect = 6;                       // 每次推理 192ms
constexpr int kMinSegmentSamples = kSampleRate * 6 / 10;   // 短于 0.6s 不值得识别
constexpr int kPollIntervalMs = 100;
}

RealtimeSubtitleWorker::RealtimeSubtitleWorker(QObject *parent)
    : QObject(parent)
{
}

RealtimeSubtitleWorker::~RealtimeSubtitleWorker()
{
    delete m_segmenter;
    delete m_vad;
}

void RealtimeSubtitleWorker::configure(const Config &cfg)
{
    m_cfg = cfg;
}

void RealtimeSubtitleWorker::setSource(PositionFn position, CopyFn copyFrom)
{
    m_position = std::move(position);
    m_copyFrom = std::move(copyFrom);
}

void RealtimeSubtitleWorker::setEngines(WhisperTranscriber *asr, Translator *translator)
{
    m_asr = asr;
    m_translator = translator;
}

void RealtimeSubtitleWorker::requestStop()
{
    m_stopRequested.store(true);
    QMetaObject::invokeMethod(this, "stop", Qt::QueuedConnection);
}

void RealtimeSubtitleWorker::start()
{
    if (m_running)
        return;
    m_stopRequested.store(false);

    if (!m_asr || !m_asr->isModelLoaded()) {
        emit errorOccurred(tr("语音识别模型未加载"));
        return;
    }
    if (!m_position || !m_copyFrom) {
        emit errorOccurred(tr("音频源未设置"));
        return;
    }

    if (!m_vad)
        m_vad = new SileroVad();
    if (!m_vad->isLoaded() && !m_vad->load(m_cfg.vadModelPath)) {
        emit errorOccurred(tr("VAD 模型加载失败：%1").arg(m_cfg.vadModelPath));
        return;
    }
    if (!m_segmenter)
        m_segmenter = new VadSegmenter();

    // 流式推理前必须清一次 LSTM 状态（新分配的 backend buffer 不保证为 0）
    m_vad->resetState();
    m_segmenter->reset();
    m_nextSample = m_position();
    m_frameBase = m_nextSample;

    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setInterval(kPollIntervalMs);
        connect(m_timer, &QTimer::timeout, this, &RealtimeSubtitleWorker::poll);
    }
    m_timer->start();
    m_running = true;

    emit statusChanged(tr("实时字幕已启动"));
}

void RealtimeSubtitleWorker::stop()
{
    if (!m_running)
        return;
    m_running = false;
    m_stopRequested.store(true);
    if (m_timer)
        m_timer->stop();
    emit statusChanged(tr("实时字幕已停止"));
}

void RealtimeSubtitleWorker::resetTimeline()
{
    if (!m_running)
        return;
    rebase(m_position());
}

void RealtimeSubtitleWorker::rebase(qint64 samplePos)
{
    if (m_vad)
        m_vad->resetState();
    if (m_segmenter)
        m_segmenter->reset();
    m_nextSample = samplePos;
    m_frameBase = samplePos;
}

void RealtimeSubtitleWorker::poll()
{
    if (!m_running || m_stopRequested.load() || !m_position || !m_copyFrom)
        return;

    const qint64 pos = m_position();

    // 环形缓冲被清空（seek/换片）后写指针会回退，此时时间线不连续
    if (pos < m_nextSample) {
        rebase(pos);
    } else if (pos - m_nextSample > qint64(kSampleRate) * m_cfg.backlogDropSec) {
        // 处理跟不上：丢掉积压，只保留最后 1 秒，避免字幕越落越远
        emit statusChanged(tr("处理落后，已丢弃积压音频"));
        rebase(pos - kSampleRate);
    }

    drain();
}

void RealtimeSubtitleWorker::drain()
{
    const qint64 pos = m_position();
    const int step = kFrameSamples * kWindowsPerDetect;

    while (pos - m_nextSample >= kFrameSamples) {
        const int want = int(std::min<qint64>(step, pos - m_nextSample));
        QVector<float> chunk = takeSamples(m_nextSample, want);
        if (chunk.size() < kFrameSamples)
            break;

        QVector<float> probs;
        if (!m_vad->detect(chunk.constData(), chunk.size(), &probs))
            break;

        QVector<VadSegmenter::Segment> segments;
        for (float p : probs)
            m_segmenter->push(p, &segments);

        m_nextSample += chunk.size();

        if (!segments.isEmpty())
            handleSegments(segments);
    }
}

QVector<float> RealtimeSubtitleWorker::takeSamples(qint64 start, qint64 count) const
{
    QVector<float> out;
    if (count <= 0)
        return out;

    out.resize(int(count));
    const int got = m_copyFrom(start, out.data(), int(count));
    out.resize(std::max(0, got));
    return out;
}

void RealtimeSubtitleWorker::handleSegments(const QVector<VadSegmenter::Segment> &segments)
{
    for (const auto &seg : segments) {
        const qint64 s0 = m_frameBase + seg.t0Frame * kFrameSamples;
        const qint64 s1 = m_frameBase + seg.t1Frame * kFrameSamples;
        const qint64 len = s1 - s0;
        if (len < kMinSegmentSamples)
            continue;

        QVector<float> audio = takeSamples(s0, len);
        if (audio.size() < kMinSegmentSamples)
            continue;   // 已被环形缓冲覆盖

        if (m_stopRequested.load())
            return;

        QElapsedTimer timer;
        timer.start();

        QString text;
        const QVector<TranscriptSegment> pieces = m_asr->transcribe(audio, m_cfg.asrLanguage);
        for (const auto &p : pieces) {
            const QString t = p.text.trimmed();
            // whisper 对静音/音乐会给 [BLANK_AUDIO]、(音乐) 这类占位
            if (t.isEmpty() || t.startsWith('[') || t.startsWith(u'('))
                continue;
            if (!text.isEmpty())
                text += ' ';
            text += t;
        }
        if (text.isEmpty())
            continue;

        if (!m_cfg.targetLang.isEmpty() && m_translator && m_translator->isModelLoaded()) {
            const QString translated = m_translator->translate(text, m_cfg.targetLang);
            if (!translated.isEmpty())
                text = translated;
        }

        const qint64 lagSamples = std::max<qint64>(0, m_position() - s1);
        emit subtitleReady(text, double(s0) / kSampleRate, double(s1) / kSampleRate,
                           double(timer.elapsed()),
                           double(lagSamples) / kSampleRate);
    }
}
