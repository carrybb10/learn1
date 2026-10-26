#pragma once

#include <QtGlobal>
#include <QVector>

// 语音活动分段状态机：吃"每帧是语音的概率"，吐出语音段（单位：帧）。
//
// 为什么要单独一个类：真实概率来自 Silero VAD 的模型推理，但"什么时候算一句话"
// 是纯逻辑——阈值、最短语音、最长静音、最长段。把它与模型解耦后，分段行为可以
// 用合成概率序列做单测，不需要模型文件，也不需要音频。
class VadSegmenter
{
public:
    struct Segment {
        qint64 t0Frame = 0;
        qint64 t1Frame = 0;   // 半开区间 [t0, t1)
    };

    struct Params {
        float threshold = 0.5f;        // 单帧判为语音的概率阈值
        int minSpeechFrames = 10;      // 少于 ~320ms 的语音不算一句（16k 下 32ms/帧）
        int minSilenceFrames = 15;     // 连续 ~480ms 静音才断句
        int maxSegmentFrames = 375;    // 超过 ~12s 强制切分，避免字幕延迟无上限
    };

    explicit VadSegmenter(const Params &params = Params());

    // 送入一帧概率；新完成的段追加到 out
    void push(float probability, QVector<Segment> *out);
    // 收尾：把未闭合的段吐出来（播放停止/切换文件时调用）
    void flush(QVector<Segment> *out);
    void reset();

    qint64 frameIndex() const { return m_frame; }
    bool inSpeech() const { return m_inSpeech; }
    const Params &params() const { return m_p; }

private:
    void emitIfLongEnough(qint64 t0, qint64 t1, QVector<Segment> *out) const;

    Params m_p;
    qint64 m_frame = 0;        // 已送入的帧数
    bool m_inSpeech = false;
    qint64 m_speechStart = 0;
    int m_silenceRun = 0;      // 语音段内连续静音帧数
};
