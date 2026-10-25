#include "Translator.h"
#include "llama.h"
#include <QDebug>
#include <QThread>
#include <QRegularExpression>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

// 辅助：向 batch 添加一个 token
static void batch_add_token(llama_batch &batch, llama_token token, llama_pos pos, llama_seq_id seq_id, bool logits) {
    batch.token[batch.n_tokens] = token;
    batch.pos[batch.n_tokens] = pos;
    batch.n_seq_id[batch.n_tokens] = 1;
    batch.seq_id[batch.n_tokens][0] = seq_id;
    batch.logits[batch.n_tokens] = logits ? 1 : 0;
    batch.n_tokens++;
}

Translator::Translator(QObject *parent)
    : QObject(parent)
{
}

Translator::~Translator()
{
    cancel();
    // 必须先 join 所有异步线程：线程体捕获了 this，只加锁不 join 会在对象析构后
    // 继续访问已释放的 llama_context / QObject → 崩溃（0xC0000005，RAX=0xdddd…）
    for (QThread *t : m_threads) {
        if (t) { t->wait(); delete t; }
    }
    m_threads.clear();

    QMutexLocker locker(&m_mutex);
    if (m_llamaCtx) {
        llama_free(m_llamaCtx);
        m_llamaCtx = nullptr;
    }
    if (m_llamaModel) {
        llama_free_model(m_llamaModel);
        m_llamaModel = nullptr;
    }
}

void Translator::reapFinishedThreads()
{
    for (int i = m_threads.size() - 1; i >= 0; --i) {
        if (m_threads[i] && m_threads[i]->isFinished()) {
            m_threads[i]->wait();
            delete m_threads[i];
            m_threads.removeAt(i);
        }
    }
}

bool Translator::loadModel(const QString &modelPath)
{
    fprintf(stderr, "[Translator] loadModel called, path: %s\n", modelPath.toUtf8().constData());
    QMutexLocker locker(&m_mutex);
    m_modelPath = modelPath;

    if (m_llamaCtx) {
        llama_free(m_llamaCtx);
        m_llamaCtx = nullptr;
    }
    if (m_llamaModel) {
        llama_free_model(m_llamaModel);
        m_llamaModel = nullptr;
    }

    llama_model_params modelParams = llama_model_default_params();
    m_llamaModel = llama_model_load_from_file(modelPath.toUtf8().constData(), modelParams);
    if (!m_llamaModel) {
        emit modelLoaded(false, "Failed to load model: " + modelPath);
        return false;
    }

    llama_context_params ctxParams = llama_context_default_params();
    ctxParams.n_ctx = 4096;
    ctxParams.n_batch = 512;
    ctxParams.n_threads = QThread::idealThreadCount();
    ctxParams.n_threads_batch = QThread::idealThreadCount();

    m_llamaCtx = llama_init_from_model(m_llamaModel, ctxParams);
    if (!m_llamaCtx) {
        llama_free_model(m_llamaModel);
        m_llamaModel = nullptr;
        emit modelLoaded(false, "Failed to create llama context");
        return false;
    }

    fprintf(stderr, "[Translator] Model loaded: %s\n", modelPath.toUtf8().constData());
    fprintf(stderr, "[Translator] Context size: %d\n", (int)llama_n_ctx(m_llamaCtx));
    emit modelLoaded(true, "");
    return true;
}

QString Translator::buildPrompt(const QString &text, const QString &targetLang)
{
    // 明确指令格式：原文 + 目标语言标签，引导模型直接输出翻译
    // 示例："Hello world\n中文：" → 模型直接接 "你好世界"
    return QString("%1\n%2：").arg(text, targetLang);
}

QString Translator::infer(const QString &prompt)
{
    // 注意：调用方（translate/translateBatch）已持有 m_mutex，此处不再加锁
    if (!m_llamaCtx || !m_llamaModel) {
        return QString();
    }

    const llama_vocab *vocab = llama_model_get_vocab(m_llamaModel);
    if (!vocab) {
        return QString();
    }

    QByteArray promptUtf8 = prompt.toUtf8();
    std::string promptStr(promptUtf8.constData(), promptUtf8.size());

    // 关键：每次翻译前清空 memory（KV cache），避免上一条字幕上下文污染
    llama_memory_clear(llama_get_memory(m_llamaCtx), true);

    // Tokenize prompt
    std::vector<llama_token> tokens(4096);
    int n_tokens = llama_tokenize(vocab, promptStr.c_str(), (int)promptStr.size(),
                                   tokens.data(), (int)tokens.size(), true, false);
    if (n_tokens <= 0) {
        qWarning() << "[Translator] Tokenize failed";
        return QString();
    }
    tokens.resize(n_tokens);

    // 创建 batch
    llama_batch batch = llama_batch_init(4096, 0, 1);

    // 添加 prompt tokens（只有最后一个需要 logits）
    for (int i = 0; i < n_tokens; i++) {
        batch_add_token(batch, tokens[i], i, 0, i == n_tokens - 1);
    }

    // 解码 prompt
    if (llama_decode(m_llamaCtx, batch) != 0) {
        qWarning() << "[Translator] Decode failed";
        llama_batch_free(batch);
        return QString();
    }

    // 创建 sampler chain — 极低温度+强重复惩罚，抑制思考过程
    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    llama_sampler *smpl = llama_sampler_chain_init(sparams);
    int n_vocab = llama_vocab_n_tokens(vocab);
    // penalties(n_vocab, penalty_last_n, penalty_repeat, penalty_freq, penalty_present)
    llama_sampler_chain_add(smpl, llama_sampler_init_penalties(n_vocab, 64, 1.4f, 0.6f, 0.4f));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(15));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.8f, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.15f));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(1234));

    // 生成循环 — 字幕翻译限制48 token（翻译结果通常很短）
    std::string output;
    int n_cur = n_tokens;
    int max_tokens = 48;
    llama_token eos_token = llama_vocab_eos(vocab);

    for (int i = 0; i < max_tokens && !m_cancelled.load(); i++) {
        // 采样
        llama_token new_token = llama_sampler_sample(smpl, m_llamaCtx, -1);

        // 检查 EOS
        if (new_token == eos_token) {
            break;
        }

        // token 转文本
        char buf[256];
        int len = llama_token_to_piece(vocab, new_token, buf, sizeof(buf), 0, false);
        if (len > 0) {
            output.append(buf, len);
        }

        // 将新 token 加入 batch 进行下一次解码
        batch.n_tokens = 0;
        batch_add_token(batch, new_token, n_cur, 0, true);
        n_cur++;

        if (llama_decode(m_llamaCtx, batch) != 0) {
            qWarning() << "[Translator] Decode failed at token" << i;
            break;
        }
    }

    // 清理
    llama_sampler_free(smpl);
    llama_batch_free(batch);

    QString result = QString::fromUtf8(output.c_str());
    result = result.trimmed();

    // 后处理1：多行输出通常包含思考过程，只保留最后一行（翻译结果）
    QStringList lines = result.split('\n', Qt::SkipEmptyParts);
    if (lines.size() > 1) {
        // 从后往前找第一个有效翻译行
        static const QStringList thinkWords = {
            "让我", "首先", "分析", "这句话", "原文", "意思是", "需要", "应该",
            "我认为", "注意", "解释", "翻译", "译文", "结果", "输出", "答案",
            "以下", "step", "first", "analyze", "because", "therefore", "think",
            "meaning", "translate", "translation", "result", "note", "however"
        };
        QString bestLine;
        for (int i = lines.size() - 1; i >= 0; i--) {
            QString line = lines[i].trimmed();
            if (line.isEmpty() || line.length() <= 1) continue;
            bool isThinking = false;
            for (const QString &w : thinkWords) {
                if (line.contains(w, Qt::CaseInsensitive)) {
                    isThinking = true;
                    break;
                }
            }
            if (!isThinking) {
                bestLine = line;
                break;
            }
        }
        if (!bestLine.isEmpty()) {
            result = bestLine;
        } else {
            result = lines.last().trimmed();
        }
    }

    // 后处理2：去除解释性前缀
    static const QStringList prefixes = {
        "翻译结果：", "翻译结果:", "翻译：", "翻译:", "译文：", "译文:",
        "以下是翻译：", "以下是翻译:", "以下是译文：", "以下是译文:",
        "翻译成中文：", "翻译成中文:", "翻译成英文：", "翻译成英文:",
        "翻译成日语：", "翻译成日语:", "翻译成韩语：", "翻译成韩语:",
        "答案：", "答案:", "结果：", "结果:", "输出：", "输出:",
        "中文：", "中文:", "英语：", "英语:", "日语：", "日语:", "韩语：", "韩语:"
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (const QString &p : prefixes) {
            if (result.startsWith(p)) {
                result = result.mid(p.length()).trimmed();
                changed = true;
                break;
            }
        }
    }
    // 去除引号包裹
    if ((result.startsWith("\"") && result.endsWith("\"") && result.length() > 2) ||
        (result.startsWith("「") && result.endsWith("」") && result.length() > 2) ||
        (result.startsWith("『") && result.endsWith("』") && result.length() > 2)) {
        result = result.mid(1, result.length() - 2).trimmed();
    }

    fprintf(stderr, "[Translator] Raw: %s\n", QString::fromUtf8(output.c_str()).trimmed().toUtf8().constData());
    fprintf(stderr, "[Translator] Final: %s\n", result.toUtf8().constData());
    return result;
}

QString Translator::translate(const QString &text, const QString &targetLang)
{
    QMutexLocker locker(&m_mutex);
    if (!m_llamaCtx) {
        return QString();
    }
    m_cancelled.store(false);

    // 过滤非语音标记：*sad music*、*music*、[music] 等
    QString filtered = text;
    filtered.remove(QRegularExpression("\\*[^*]*\\*"));
    filtered.remove(QRegularExpression("\\[[^\\]]*\\]"));
    filtered = filtered.trimmed();
    if (filtered.isEmpty()) {
        fprintf(stderr, "[Translator] Skip empty/non-speech text: %s\n", text.toUtf8().constData());
        return QString();
    }

    fprintf(stderr, "[Translator] Translating [%s] -> [%s]: %s\n",
            targetLang.toUtf8().constData(),
            filtered.toUtf8().constData(),
            "");

    QString prompt = buildPrompt(filtered, targetLang);
    QString result = infer(prompt);

    // 后处理：去除原文重复（模型有时会把原文也输出）
    if (!result.isEmpty() && filtered.length() > 2) {
        if (result.contains(filtered, Qt::CaseInsensitive)) {
            result = result.remove(filtered, Qt::CaseInsensitive).trimmed();
        }
    }

    // 后处理：去除目标语言标签残留（如"中文："、"英语:"等）
    static const QStringList langLabels = {
        "中文：", "中文:", "英语：", "英语:", "日语：", "日语:", "韩语：", "韩语:",
        "Chinese:", "English:", "Japanese:", "Korean:",
        "翻译：", "翻译:", "译文：", "译文:"
    };
    for (const QString &label : langLabels) {
        if (result.startsWith(label)) {
            result = result.mid(label.length()).trimmed();
            break;
        }
    }

    // 后处理：多语言混合过滤 — 根据目标语言保留主要字符集
    if (!result.isEmpty()) {
        QString filteredResult;
        bool hasTarget = false;
        for (QChar c : result) {
            ushort unicode = c.unicode();
            bool keep = true;
            if (targetLang == "中文") {
                // 中文：保留中文、标点、数字、英文（外来词），过滤日文假名和韩文
                if ((unicode >= 0x3040 && unicode <= 0x30FF) ||  // 日文假名
                    (unicode >= 0xAC00 && unicode <= 0xD7AF)) {  // 韩文
                    keep = false;
                }
            } else if (targetLang == "日语") {
                // 日语：保留日文假名、汉字、标点、数字，过滤韩文
                if (unicode >= 0xAC00 && unicode <= 0xD7AF) {  // 韩文
                    keep = false;
                }
            } else if (targetLang == "英语") {
                // 英语：保留英文、标点、数字，过滤中日韩文字（简单处理）
                if ((unicode >= 0x3040 && unicode <= 0x30FF) ||  // 日文假名
                    (unicode >= 0xAC00 && unicode <= 0xD7AF) ||  // 韩文
                    (unicode >= 0x4E00 && unicode <= 0x9FFF)) {  // 中文汉字
                    keep = false;
                }
            } else if (targetLang == "韩语") {
                // 韩语：保留韩文、标点、数字，过滤日文假名和中文汉字
                if ((unicode >= 0x3040 && unicode <= 0x30FF) ||  // 日文假名
                    (unicode >= 0x4E00 && unicode <= 0x9FFF)) {  // 中文汉字
                    keep = false;
                }
            }
            if (keep) {
                filteredResult += c;
                if (targetLang == "中文" && (unicode >= 0x4E00 && unicode <= 0x9FFF)) hasTarget = true;
                if (targetLang == "日语" && (unicode >= 0x3040 && unicode <= 0x30FF)) hasTarget = true;
                if (targetLang == "英语" && ((unicode >= 'A' && unicode <= 'Z') || (unicode >= 'a' && unicode <= 'z'))) hasTarget = true;
                if (targetLang == "韩语" && (unicode >= 0xAC00 && unicode <= 0xD7AF)) hasTarget = true;
            }
        }
        // 如果过滤后还有目标语言字符，使用过滤结果；否则保留原结果
        if (hasTarget && filteredResult.trimmed().length() > 0) {
            result = filteredResult.trimmed();
        }
    }

    return result;
}

QVector<TranslationResult> Translator::translateBatch(const QVector<QString> &texts, const QString &targetLang)
{
    return translateBatchTask(texts, targetLang, -1);
}

QVector<TranslationResult> Translator::translateBatchTask(const QVector<QString> &texts,
                                                          const QString &targetLang, int taskId)
{
    QVector<TranslationResult> results;
    for (const QString &text : texts) {
        TranslationResult r;
        r.sourceText = text;
        r.targetLang = targetLang;
        // taskId>=0 表示异步任务：被更新任务抢占即取消（不再重置全局 m_cancelled，
        // 否则旧任务会把自己"复活"）；taskId=-1 表示同步调用，只看 cancel()
        const bool cancelled = m_cancelled.load() ||
                               (taskId >= 0 && m_currentTaskId.load() != taskId);
        if (cancelled) {
            r.success = false;
            r.error = "Cancelled";
        } else {
            r.translatedText = translate(text, targetLang);
            r.success = !r.translatedText.isEmpty();
            if (!r.success) r.error = "Translation failed";
        }
        results.append(r);
    }
    return results;
}

void Translator::translateAsync(const QString &text, const QString &targetLang)
{
    reapFinishedThreads();
    QThread *thread = QThread::create([this, text, targetLang]() {
        TranslationResult result;
        result.sourceText = text;
        result.targetLang = targetLang;
        result.translatedText = translate(text, targetLang);
        result.success = !result.translatedText.isEmpty();
        if (!result.success) result.error = "Translation failed";
        emit translateFinished(result);
    });
    m_threads.append(thread);
    thread->start();
}

void Translator::translateBatchAsync(const QVector<QString> &texts, const QString &targetLang)
{
    // 生成新任务ID，旧任务完成时会检测到ID不匹配而不发送结果
    int taskId = ++m_currentTaskId;
    m_cancelled.store(false);   // 只服务于新任务；旧任务靠 taskId 不匹配退出

    reapFinishedThreads();
    QThread *thread = QThread::create([this, texts, targetLang, taskId]() {
        QVector<TranslationResult> results = translateBatchTask(texts, targetLang, taskId);
        // 只有当前任务ID匹配时才发送结果（避免旧任务覆盖新任务）
        if (m_currentTaskId.load() == taskId && !m_cancelled.load()) {
            emit batchTranslateFinished(results);
        }
    });
    m_threads.append(thread);
    thread->start();
}

void Translator::cancel()
{
    m_cancelled.store(true);
}
