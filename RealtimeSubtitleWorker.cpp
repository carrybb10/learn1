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
constexpr int kWindowsPerDetect = 30;                      // 每次推理 960ms：VAD 每次调用有 40-100ms 固定开销，窗口越小越吃亏
constexpr int kMinSegmentSamples = kSampleRate * 6 / 10;   // 短于 0.6s 不值得识别
constexpr int kPollIntervalMs = 100;

// 句尾标点判定：中文/日文也要认，whisper 对短片段常常不给标点
bool endsWithSentencePunct(const QString &text)
{
    if (text.isEmpty())
        return false;
    const QChar c = text.at(text.size() - 1);
    return QStringLiteral(".?!。！？…").contains(c);
}
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
    // 停止时丢掉还没攒完的半句：不再为它等一次秒级识别
    m_pendingAudio.clear();
    m_carryText.clear();
    m_carryEnd = -1;
    m_lastSegmentEnd = -1;
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
    // 时间线断了，攒到一半的整句不能再和后面的音频拼在一起
    m_pendingAudio.clear();
    m_lastSegmentEnd = -1;
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

    // 说话停了（或音源结束，写指针不再前进）：把攒着的整句送出去
    // 注意必须先看分段器是不是还在语音里：VAD 要等静音才吐出段，正在说的那段还没结算
    const qint64 gapSamples = qint64(kSampleRate) * m_cfg.sentenceGapMs / 1000;
    if (!m_segmenter->inSpeech() && !m_pendingAudio.isEmpty() && m_lastSegmentEnd >= 0
        && pos - m_lastSegmentEnd >= gapSamples)
        flushUtterance();

    // 攒着的半句等不到下一句（例如后面是音乐/静音）也要落地，否则永远不出字幕
    if (!m_carryText.isEmpty() && m_carryEnd >= 0 && pos - m_carryEnd >= gapSamples)
        flushCarry();
}

void RealtimeSubtitleWorker::drain()
{
    const qint64 pos = m_position();
    const int step = kFrameSamples * kWindowsPerDetect;

    // 只在攒够一整块时才调 VAD：每次调用有 40-100ms 固定开销（graph 重建 + 调度），
    // 按"当前到了多少就喂多少"会每 100ms 付一次，等于 1x 实时，永远追不上播放。
    while (pos - m_nextSample >= step) {
        const int want = step;
        QVector<float> chunk = takeSamples(m_nextSample, want);
        if (chunk.size() < step)
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
    const qint64 gapSamples = qint64(kSampleRate) * m_cfg.sentenceGapMs / 1000;
    const qint64 maxSamples = qint64(kSampleRate) * m_cfg.maxUtteranceMs / 1000;

    for (const auto &seg : segments) {
        const qint64 s0 = m_frameBase + seg.t0Frame * kFrameSamples;
        const qint64 s1 = m_frameBase + seg.t1Frame * kFrameSamples;
        if (s1 - s0 < kMinSegmentSamples)
            continue;

        // 段间停顿够长说明上一句说完了：先把攒好的整句送出去，别把两句拼一起
        if (!m_pendingAudio.isEmpty() && m_lastSegmentEnd >= 0 && s0 - m_lastSegmentEnd >= gapSamples)
            flushUtterance();

        if (m_stopRequested.load())
            return;

        appendToUtterance(s0, s1);

        // 一直不停顿的长句强制切：否则字幕要等到下一句才开始出
        if (qint64(m_pendingAudio.size()) >= maxSamples)
            flushUtterance();
    }
}

void RealtimeSubtitleWorker::appendToUtterance(qint64 s0, qint64 s1)
{
    if (s1 <= s0)
        return;
    const QVector<float> audio = takeSamples(s0, s1 - s0);
    if (audio.isEmpty())
        return;
    if (m_pendingAudio.isEmpty())
        m_pendingStart = s0;
    m_pendingAudio += audio;
    m_pendingEnd = m_pendingStart + m_pendingAudio.size();
    m_lastSegmentEnd = s1;
}

void RealtimeSubtitleWorker::flushUtterance()
{
    if (m_pendingAudio.size() < kMinSegmentSamples) {
        m_pendingAudio.clear();
        m_lastSegmentEnd = -1;
        return;
    }

    QVector<float> audio;
    audio.swap(m_pendingAudio);
    const qint64 s0 = m_pendingStart;
    const qint64 s1 = s0 + audio.size();
    m_pendingEnd = s1;
    m_lastSegmentEnd = -1;

    if (m_stopRequested.load())
        return;   // 退出时不再等一次几秒的识别

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
        return;

    // 识别出来的常常是半句（VAD 在换气处断开）：攒到句号/问号或够长再翻，
    // 否则翻译模型只看到 "can do for you," 这种碎片，译文必定跑偏。
    if (m_carryText.isEmpty())
        m_carryStart = s0;
    const QString combined = m_carryText.isEmpty() ? text : m_carryText + ' ' + text;
    const bool terminated = endsWithSentencePunct(text);
    const bool longEnough = combined.size() >= 48 || (s1 - m_carryStart) >= qint64(kSampleRate) * 6;
    if (!terminated && !longEnough) {
        m_carryText = combined;
        m_carryEnd = s1;
        return;
    }
    m_carryText.clear();
    m_carryEnd = -1;

    const qint64 startSample = m_carryStart;
    if (!m_cfg.targetLang.isEmpty() && m_translator && m_translator->isModelLoaded()) {
        const QString translated = m_translator->translate(combined, m_cfg.targetLang);
        if (!translated.isEmpty())
            text = translated;
        else
            text = combined;
    } else {
        text = combined;
    }

    const qint64 lagSamples = std::max<qint64>(0, m_position() - s1);
    emit subtitleReady(text, double(startSample) / kSampleRate, double(s1) / kSampleRate,
                       double(timer.elapsed()),
                       double(lagSamples) / kSampleRate);
}

// 把攒着的半句单独送出去：句尾没有标点，也不能让它一直挂着不出字幕。
void RealtimeSubtitleWorker::flushCarry()
{
    if (m_carryText.isEmpty())
        return;

    const QString text = m_carryText;
    const qint64 startSample = m_carryStart;
    const qint64 endSample = m_carryEnd >= 0 ? m_carryEnd : m_carryStart;
    m_carryText.clear();
    m_carryEnd = -1;

    QString out = text;
    if (!m_cfg.targetLang.isEmpty() && m_translator && m_translator->isModelLoaded()) {
        const QString translated = m_translator->translate(text, m_cfg.targetLang);
        if (!translated.isEmpty())
            out = translated;
    }

    const qint64 lagSamples = std::max<qint64>(0, m_position() - endSample);
    emit subtitleReady(out, double(startSample) / kSampleRate, double(endSample) / kSampleRate,
                       0.0, double(lagSamples) / kSampleRate);
}
