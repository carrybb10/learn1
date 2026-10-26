#include "AudioRingBuffer.h"

AudioRingBuffer::AudioRingBuffer(int capacitySamples)
    : m_buf(capacitySamples > 0 ? capacitySamples : 1)
{
    m_buf.fill(0.0f);
}

void AudioRingBuffer::push(const float *data, int count)
{
    if (!data || count <= 0) {
        return;
    }

    const int cap = m_buf.size();
    // 一次写入超过容量时，只有最后 cap 个样本有意义，但绝对位置要按写入总量推进。
    const int skip = count > cap ? count - cap : 0;
    const int keep = count - skip;

    int idx = int((m_written + skip) % cap);
    for (int i = 0; i < keep; ++i) {
        m_buf[idx] = data[skip + i];
        if (++idx == cap) {
            idx = 0;
        }
    }
    m_written += count;
}

int AudioRingBuffer::availableFrom(qint64 absStart) const
{
    const qint64 start = qMax(absStart, oldestPosition());
    if (start >= m_written) {
        return 0;
    }
    return int(m_written - start);
}

int AudioRingBuffer::copyFrom(qint64 absStart, float *out, int maxCount) const
{
    if (!out || maxCount <= 0) {
        return 0;
    }

    const int cap = m_buf.size();
    const qint64 start = qMax(absStart, oldestPosition());
    if (start >= m_written) {
        return 0;
    }

    const int n = int(qMin<qint64>(maxCount, m_written - start));
    int idx = int(start % cap);
    for (int i = 0; i < n; ++i) {
        out[i] = m_buf[idx];
        if (++idx == cap) {
            idx = 0;
        }
    }
    return n;
}

void AudioRingBuffer::clear()
{
    m_buf.fill(0.0f);
    m_written = 0;
}
