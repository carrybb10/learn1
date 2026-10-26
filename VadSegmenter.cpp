#include "VadSegmenter.h"

VadSegmenter::VadSegmenter(const Params &params)
    : m_p(params)
{
    if (m_p.minSpeechFrames < 1)
        m_p.minSpeechFrames = 1;
    if (m_p.minSilenceFrames < 1)
        m_p.minSilenceFrames = 1;
    if (m_p.maxSegmentFrames < m_p.minSpeechFrames)
        m_p.maxSegmentFrames = m_p.minSpeechFrames;
}

void VadSegmenter::emitIfLongEnough(qint64 t0, qint64 t1, QVector<Segment> *out) const
{
    if (t1 - t0 < m_p.minSpeechFrames)
        return;
    if (out)
        out->append(Segment{t0, t1});
}

void VadSegmenter::push(float probability, QVector<Segment> *out)
{
    const qint64 frame = m_frame++;

    if (!m_inSpeech) {
        if (probability >= m_p.threshold) {
            m_inSpeech = true;
            m_speechStart = frame;
            m_silenceRun = 0;
        }
        return;
    }

    if (probability >= m_p.threshold) {
        m_silenceRun = 0;
        const qint64 len = frame + 1 - m_speechStart;
        if (len >= m_p.maxSegmentFrames) {
            // 一个人不停说下去时强制切，否则字幕会一直不出来
            emitIfLongEnough(m_speechStart, frame + 1, out);
            m_inSpeech = false;
            m_silenceRun = 0;
        }
        return;
    }

    ++m_silenceRun;
    if (m_silenceRun >= m_p.minSilenceFrames) {
        // 断句点回退到静音开始处，别把尾巴上的静音算进语音段
        const qint64 speechEnd = frame + 1 - m_silenceRun;
        emitIfLongEnough(m_speechStart, speechEnd, out);
        m_inSpeech = false;
        m_silenceRun = 0;
    }
}

void VadSegmenter::flush(QVector<Segment> *out)
{
    if (!m_inSpeech)
        return;
    emitIfLongEnough(m_speechStart, m_frame, out);
    m_inSpeech = false;
    m_silenceRun = 0;
}

void VadSegmenter::reset()
{
    m_frame = 0;
    m_inSpeech = false;
    m_speechStart = 0;
    m_silenceRun = 0;
}
