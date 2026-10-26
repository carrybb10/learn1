#pragma once

#include <QtGlobal>
#include <QVector>

// 定长环形缓冲，存 16kHz mono float32（实时字幕的音频来源）。
//
// 为什么需要它：原来的 AudioTap 用无上限 QVector 累积整段音频，1 小时约 230MB，
// 而且识别必须等到"录完"才开始。改成环形缓冲后：
//   - 内存恒定（默认 60 秒 = 3.84MB）
//   - 消费者按"绝对样本位置"取数据，可以边播边识别
// 线程安全由调用方保证：PlayerController 在 m_audioTapMutex 下调用。
class AudioRingBuffer
{
public:
    explicit AudioRingBuffer(int capacitySamples = 16000 * 60);

    // 写入样本；超过容量时丢弃最旧的数据。writePosition() 仍然按写入总量推进。
    void push(const float *data, int count);
    // 从绝对位置 absStart 起拷贝最多 maxCount 个样本，返回实际拷贝数。
    int copyFrom(qint64 absStart, float *out, int maxCount) const;
    // 该位置起还有多少样本可取（已被覆盖的返回 0）。
    int availableFrom(qint64 absStart) const;

    qint64 writePosition() const { return m_written; }
    qint64 oldestPosition() const { return qMax<qint64>(0, m_written - m_buf.size()); }
    int capacity() const { return m_buf.size(); }
    void clear();

private:
    QVector<float> m_buf;
    qint64 m_written = 0;
};
