#pragma once

#include <QString>
#include <QVector>

struct whisper_vad_context;

// Silero VAD 的薄封装：只做"加载模型 + 流式给概率"，分段策略在 VadSegmenter 里。
//
// 流式推理用 whisper_vad_detect_speech_no_reset：它不会清 LSTM 状态，适合一路喂
// 连续的音频；断句/换片时由调用方 resetState()。
// 本类不是线程安全的，只由一个消费者线程持有。
class SileroVad
{
public:
    // Silero v5/v6 的推理窗口：512 样本，16k 下等于 32ms
    static constexpr int kWindowSamples = 512;

    SileroVad();
    ~SileroVad();

    SileroVad(const SileroVad &) = delete;
    SileroVad &operator=(const SileroVad &) = delete;

    bool load(const QString &modelPath);
    void unload();
    bool isLoaded() const { return m_ctx != nullptr; }

    // 送入任意长度的 16k 单声道样本，把逐窗语音概率追加到 probs
    bool detect(const float *samples, int count, QVector<float> *probs);

    // 一次性模式（参考实现）：先清 LSTM 状态再整段推理，用于和流式结果对拍
    bool detectWhole(const float *samples, int count, QVector<float> *probs);

    void resetState();

private:
    whisper_vad_context *m_ctx = nullptr;
};
