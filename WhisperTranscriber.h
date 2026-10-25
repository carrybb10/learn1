#ifndef WHISPERTRANSCRIBER_H
#define WHISPERTRANSCRIBER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QThread>
#include <QMutex>
#include <atomic>

struct TranscriptSegment {
    double start = 0;  // 秒
    double end = 0;    // 秒
    QString text;
};

class WhisperTranscriber : public QObject
{
    Q_OBJECT
public:
    explicit WhisperTranscriber(QObject *parent = nullptr);
    ~WhisperTranscriber();

    // 加载模型（异步，加载完成后发出 modelLoaded 信号）
    bool loadModel(const QString &modelPath);
    bool isModelLoaded() const { return m_whisperCtx != nullptr; }

    // 同步识别（阻塞调用，建议在后台线程使用）
    // samples: 16kHz mono float32 音频数据
    QVector<TranscriptSegment> transcribe(const QVector<float> &samples, const QString &language = "auto");

    // 异步识别
    void transcribeAsync(const QVector<float> &samples, const QString &language = "auto");

    // 取消识别
    void cancel();

signals:
    void modelLoaded(bool success, const QString &error);
    void transcribeFinished(const QVector<TranscriptSegment> &segments);
    void transcribeProgress(int progress);  // 0-100
    void errorOccurred(const QString &error);

private:
    struct whisper_context *m_whisperCtx = nullptr;
    QString m_modelPath;
    std::atomic<bool> m_cancelled{false};
    QMutex m_mutex;
};

#endif // WHISPERTRANSCRIBER_H
