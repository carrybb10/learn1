#ifndef TRANSLATOR_H
#define TRANSLATOR_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QByteArray>
#include <QMutex>
#include <atomic>

class QThread;

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

    // 加载模型（同步，阻塞调用；内部持 m_mutex）
    bool loadModel(const QString &modelPath);
    // 加载模型（异步，加载完成后发出 modelLoaded 信号）。线程登记进 m_threads，析构时 join。
    void loadModelAsync(const QString &modelPath);
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

    // 构建纯文本翻译指令（无 chat 模板时的降级路径）
    static QString buildPrompt(const QString &text, const QString &targetLang);

    // 判断文本本身是否已经是目标语言（是则无需翻译，直接透传）
    static bool looksLikeLanguage(const QString &text, const QString &targetLang);

    // 清理模型输出：去 chat 标记、去思考行、去标签前缀、去引号
    static QString postProcess(const QString &raw, const QString &targetLang);

signals:
    void modelLoaded(bool success, const QString &error);
    void translateFinished(const TranslationResult &result);
    void batchTranslateFinished(const QVector<TranslationResult> &results);
    void errorOccurred(const QString &error);

private:
    QVector<TranslationResult> translateBatchTask(const QVector<QString> &texts,
                                                  const QString &targetLang, int taskId);
    void reapFinishedThreads();          // 回收已结束的异步线程，避免句柄堆积

    struct llama_context *m_llamaCtx = nullptr;
    struct llama_model *m_llamaModel = nullptr;
    QString m_modelPath;
    std::atomic<bool> m_cancelled{false};
    std::atomic<int> m_currentTaskId{0};  // 任务ID：用于区分新旧翻译任务，避免结果交错
    QMutex m_mutex;
    QVector<QThread*> m_threads;          // 异步线程；析构时必须全部 join
    QByteArray m_chatTemplate;            // 模型自带 chat 模板（gguf 元数据），为空则用 ChatML

    // 内部推理函数
    QString buildChatPrompt(const QString &text, const QString &targetLang) const;
    QString infer(const QString &prompt);
};

#endif // TRANSLATOR_H
