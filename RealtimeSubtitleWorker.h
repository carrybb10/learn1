#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>

#include "VadSegmenter.h"

class QTimer;
class SileroVad;
class WhisperTranscriber;
class Translator;

// 实时字幕工作线程：环形缓冲 -> Silero VAD 分段 -> whisper 识别 -> （可选）翻译 -> 发信号
//
// 设计约束：
// - 只读环形缓冲，不碰播放状态；seek 由 resetTimeline() 通知（时间线不连续）。
// - whisper / translator 用应用里已有的实例（不重复加载 1GB 模型），靠它们自身的 mutex 串行。
// - 处理跟不上时丢弃积压（默认 10s），宁可丢句子也不让字幕越落越远。
class RealtimeSubtitleWorker : public QObject
{
    Q_OBJECT
public:
    // 取环形缓冲的写指针 / 按绝对样本位置复制数据（实现方负责加锁）
    using PositionFn = std::function<qint64()>;
    using CopyFn = std::function<int(qint64 absStart, float *out, int maxCount)>;

    struct Config {
        QString vadModelPath;
        QString asrLanguage = QStringLiteral("auto");
        QString targetLang;        // 空 = 不翻译
        int backlogDropSec = 10;
    };

    explicit RealtimeSubtitleWorker(QObject *parent = nullptr);
    // 任意线程可调用：置停止标志并投递 stop（stop 内部要停 QTimer，必须在本线程执行）
    void requestStop();
    bool stopRequested() const { return m_stopRequested.load(); }
    ~RealtimeSubtitleWorker() override;

    // 以下两个必须在 start() 之前、且与 start() 同线程调用（一般就在 moveToThread 之前）
    void configure(const Config &cfg);
    void setSource(PositionFn position, CopyFn copyFrom);
    void setEngines(WhisperTranscriber *asr, Translator *translator);

public slots:
    void start();            // 在 worker 线程里跑：加载 VAD、起定时器
    void stop();
    void resetTimeline();    // seek/换片：VAD 状态与帧基准都要清

signals:
    void subtitleReady(const QString &text, double t0sec, double t1sec, double procMs, double lagSec);
    void statusChanged(const QString &status);
    void errorOccurred(const QString &error);

private slots:
    void poll();

private:
    void drain();
    void handleSegments(const QVector<VadSegmenter::Segment> &segments);
    QVector<float> takeSamples(qint64 start, qint64 count) const;
    void rebase(qint64 samplePos);

    Config m_cfg;
    PositionFn m_position;
    CopyFn m_copyFrom;
    WhisperTranscriber *m_asr = nullptr;
    Translator *m_translator = nullptr;

    QTimer *m_timer = nullptr;
    SileroVad *m_vad = nullptr;
    VadSegmenter *m_segmenter = nullptr;

    qint64 m_nextSample = 0;   // 下一个待送 VAD 的绝对样本位置
    qint64 m_frameBase = 0;    // 第 0 帧对应的绝对样本位置
    bool m_running = false;
    std::atomic<bool> m_stopRequested{false};
};
