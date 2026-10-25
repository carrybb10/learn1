#ifndef TRANSLATOR_H
#define TRANSLATOR_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QMutex>
#include <atomic>

struct TranslationResult {
    QString sourceText;
    QString translatedText;
    QString targetLang;
    bool success = false;
    QString error;
};

class Translator : public QObject
{
    Q_OBJECT
public:
    explicit Translator(QObject *parent = nullptr);
    ~Translator();

    // 加载模型（异步，加载完成后发出 modelLoaded 信号）
    bool loadModel(const QString &modelPath);
    bool isModelLoaded() const { return m_llamaCtx != nullptr; }

    // 同步翻译（阻塞调用，建议在后台线程使用）
    // targetLang: "中文" "英语" "日语" "韩语" 等
    QString translate(const QString &text, const QString &targetLang = "中文");

    // 批量同步翻译（返回与输入等长的结果列表）
    QVector<TranslationResult> translateBatch(const QVector<QString> &texts, const QString &targetLang = "中文");

    // 异步翻译
    void translateAsync(const QString &text, const QString &targetLang = "中文");

    // 异步批量翻译
    void translateBatchAsync(const QVector<QString> &texts, const QString &targetLang = "中文");

    // 取消翻译
    void cancel();

    // 构建翻译 prompt
    static QString buildPrompt(const QString &text, const QString &targetLang);

signals:
    void modelLoaded(bool success, const QString &error);
    void translateFinished(const TranslationResult &result);
    void batchTranslateFinished(const QVector<TranslationResult> &results);
    void errorOccurred(const QString &error);

private:
    struct llama_context *m_llamaCtx = nullptr;
    struct llama_model *m_llamaModel = nullptr;
    QString m_modelPath;
    std::atomic<bool> m_cancelled{false};
    std::atomic<int> m_currentTaskId{0};  // 任务ID：用于区分新旧翻译任务，避免结果交错
    QMutex m_mutex;

    // 内部推理函数
    QString infer(const QString &prompt);
};

#endif // TRANSLATOR_H
