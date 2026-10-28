#include "Translator.h"
#include "llama.h"
#include <QDebug>
#include <QThread>
#include <QRegularExpression>
#include <QSet>
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

void Translator::loadModelAsync(const QString &modelPath)
{
    reapFinishedThreads();
    QThread *t = QThread::create([this, modelPath]() { loadModel(modelPath); });
    m_threads.append(t);
    t->start();
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
    // 翻译层放到 GPU 上：llama.cpp 开了 GGML_CUDA 时 n_gpu_layers>0 才真正上卡，
    // CPU 版本会忽略这个值。显存吃紧时用 MP_MT_GPU_LAYERS 调小或设 0 退回 CPU。
    {
        bool ok = false;
        int layers = qEnvironmentVariableIntValue("MP_MT_GPU_LAYERS", &ok);
        modelParams.n_gpu_layers = (ok && layers >= 0) ? layers : 99;
        fprintf(stderr, "[Translator] n_gpu_layers = %d\n", modelParams.n_gpu_layers);
    }
    m_llamaModel = llama_model_load_from_file(modelPath.toUtf8().constData(), modelParams);
    if (!m_llamaModel) {
        emit modelLoaded(false, "Failed to load model: " + modelPath);
        return false;
    }

    // 取模型自带的 chat 模板：翻译模型是 instruct 模型，必须按它的对话格式提问，
    // 否则模型只会把 "原文 + 语言标签" 当普通文本续写（这是译文跑偏的根因）
    const char *tmpl = llama_model_chat_template(m_llamaModel, nullptr);
    m_chatTemplate = (tmpl && *tmpl) ? QByteArray(tmpl) : QByteArray();
    fprintf(stderr, "[Translator] chat template: %s\n",
            m_chatTemplate.isEmpty() ? "<fallback ChatML>" : "from gguf");
    if (qEnvironmentVariableIsSet("TRANSLATOR_DEBUG_TEMPLATE")) {
        fprintf(stderr, "---- template begin ----\n%s\n---- template end ----\n",
                m_chatTemplate.constData());
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
    // 纯文本降级路径：没有对话模板时也得把"只输出译文"说清楚
    return QStringLiteral("请将以下文本翻译为%1。只需要输出翻译后的结果，不要额外解释：\n\n%2")
        .arg(targetLang, text);
}

QString Translator::buildChatPrompt(const QString &text, const QString &targetLang) const
{
    // 指令放在 user 里，不给 system —— Hy-MT2 官方 model card 明确说模型没有默认
    // system_prompt，且所有示例（默认/风格/术语/背景）都把任务写进 user。
    // 风格用官方 Style 模板的写法；中文目标必须写明"简体"，否则会出繁体。
    QString userPrompt;
    if (targetLang == "中文") {
        userPrompt = QStringLiteral(
            "请将以下文本翻译为简体中文（大陆规范汉字）。"
            "注意翻译的风格要严格符合【口语化、符合中文字幕习惯、简洁自然，不要逐字直译】。"
            "只需要输出翻译后的结果，不要额外解释，不要保留原文。\n\n%1").arg(text);
    } else {
        userPrompt = QStringLiteral(
            "Please translate the following text into %1. "
            "The translation style must strictly conform to "
            "[colloquial, natural spoken %1, concise subtitle style, not word-for-word]. "
            "Only output the translated result without any additional explanation.\n\n%2")
            .arg(targetLang, text);
    }

    // 1. Hunyuan-MT 系（HY-MT1.5 / HY-MT2）：它的对话格式是自定义特殊 token，
    //    llama_chat_apply_template 的预置模板表里没有，必须手工拼。
    if (m_chatTemplate.contains(QStringLiteral("<｜hy_User｜>").toUtf8())) {
        return QStringLiteral("<｜hy_begin▁of▁sentence｜><｜hy_User｜>%1<｜hy_Assistant｜>")
            .arg(userPrompt);
    }

    // 2. 其它模型：让 llama.cpp 用它认识的预置模板（只发一条 user）
    if (!m_chatTemplate.isEmpty()) {
        QByteArray userU = userPrompt.toUtf8();
        llama_chat_message msgs[1];
        msgs[0].role = "user";
        msgs[0].content = userU.constData();

        QByteArray tmpl = m_chatTemplate;
        std::vector<char> buf((size_t)(4 * userU.size() + 1024), '\0');
        int n = llama_chat_apply_template(tmpl.constData(), msgs, 1, true, buf.data(), (int)buf.size());
        if (n > 0 && (size_t)n <= buf.size()) {
            return QString::fromUtf8(buf.data(), n);
        }
        fprintf(stderr, "[Translator] chat template apply failed (n=%d), fallback to ChatML\n", n);
    }

    // 3. ChatML 兜底（Qwen 系）
    return QStringLiteral("<|im_start|>user\n%1<|im_end|>\n"
                          "<|im_start|>assistant\n").arg(userPrompt);
}

bool Translator::looksLikeLanguage(const QString &text, const QString &lang)
{
    int cjk = 0, kana = 0, hangul = 0, latin = 0, total = 0;
    for (QChar c : text) {
        ushort u = c.unicode();
        if (u <= ' ') continue;
        total++;
        if (u >= 0x3040 && u <= 0x30FF)      kana++;
        else if (u >= 0xAC00 && u <= 0xD7AF) hangul++;
        else if (u >= 0x4E00 && u <= 0x9FFF) cjk++;
        else if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')) latin++;
    }
    if (total == 0) return false;

    if (lang == "中文") return kana == 0 && hangul == 0 && cjk * 2 >= total;
    // 日文必须出现假名才算"已经是日语"，否则纯汉字的原文（中文）会被误判成日语
    if (lang == "日语") return kana > 0 && hangul == 0 && kana + cjk >= total / 2;
    if (lang == "韩语") return hangul > 0 && kana == 0 && cjk == 0;

    // 拉丁字母不区分语种（法语/德语/西语都是 ASCII），只看字符集会把法语当成英语
    // 直接透传，所以这里用高频虚词做一次轻量语言判定。
    if (lang == "英语") {
        if (latin * 2 < total || cjk || kana || hangul) return false;

        static const QSet<QString> enWords = {
            "the","and","you","we","is","are","was","were","to","of","in","on","at","for","with",
            "that","this","it","not","have","has","had","be","been","do","does","did","but","or",
            "if","my","your","his","her","our","their","what","when","where","why","how","who",
            "she","he","they","i","me","him","them","us","no","yes","just","now","then","there",
            "here","can","will","would","should","could","get","got","know","think","want","need",
            "never","thought","make","far","signal","waiting","since","noon","station","much","time"
        };
        static const QSet<QString> frWords = {
            "le","la","les","des","du","un","une","et","est","pas","plus","dans","pour","avec",
            "nous","vous","ils","elles","je","tu","il","elle","que","qui","quoi","ne","ce","cette",
            "ces","sur","au","aux","mais","ou","donc","car","tres","peut","etre","avoir","fait",
            "tout","bien","temps","jour","beaucoup","notre","votre","leur","sont","etait"
        };

        int en = 0, fr = 0;
        const QStringList words = text.toLower()
            .split(QRegularExpression("[^A-Za-z]+"), Qt::SkipEmptyParts);
        for (const QString &w : words) {
            if (enWords.contains(w)) en++;
            if (frWords.contains(w)) fr++;
        }
        return en > 0 && en > fr;
    }
    return false;
}

QString Translator::postProcess(const QString &raw, const QString &targetLang)
{
    Q_UNUSED(targetLang);
    QString text = raw;

    // 1. 截断 chat 结束标记（模型可能把控制符也吐出来）
    for (const char *marker : { "<|im_end|>", "<|eot_id|>", "<|endoftext|>", "<|end|>" }) {
        int p = text.indexOf(QLatin1String(marker));
        if (p >= 0) text.truncate(p);
    }

    // 2. 单句字幕：只取第一条有效行。旧版"从后往前找最后一行"会把模型
    //    额外续写的 "**Translation:** ..." 当成译文，是本项目的老毛病。
    static const QRegularExpression labelRe(
        "^\\s*(\\*\\*)?\\s*(translation|translate|译文|翻译|output|result|结果)\\s*(\\*\\*)?\\s*[:：]\\s*",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression boldRe("^\\*+|\\*+$");

    QString result;
    const QStringList lines = text.split('\n');
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        line.replace(labelRe, QString());
        line.replace(boldRe, QString());
        line = line.trimmed();
        if (line.isEmpty() || line.length() <= 1) continue;
        result = line;
        break;
    }
    if (result.isEmpty()) result = text.trimmed();

    // 3. 去掉解释性前缀（"译文：" 等），逐个剥离
    static const QStringList prefixes = {
        "翻译结果：", "翻译结果:", "翻译：", "翻译:", "译文：", "译文:",
        "以下是翻译：", "以下是翻译:", "以下是译文：", "以下是译文:",
        "翻译成中文：", "翻译成中文:", "翻译成英文：", "翻译成英文:",
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

    // 4. 去掉包裹引号
    if ((result.startsWith('"') && result.endsWith('"') && result.length() > 2) ||
        (result.startsWith("「") && result.endsWith("」") && result.length() > 2) ||
        (result.startsWith("『") && result.endsWith("』") && result.length() > 2)) {
        result = result.mid(1, result.length() - 2).trimmed();
    }
    return result;
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
    // 最后两个参数：add_special / parse_special。
    // parse_special 必须为 true，否则 <｜hy_User｜> 这类对话控制符会被当作普通字符
    // 拆成碎片，模型根本看不到对话结构 —— 这是"译不出/乱续写"的直接原因。
    int n_tokens = llama_tokenize(vocab, promptStr.c_str(), (int)promptStr.size(),
                                   tokens.data(), (int)tokens.size(), true, true);
    if (n_tokens <= 0) {
        qWarning() << "[Translator] Tokenize failed";
        return QString();
    }
    tokens.resize(n_tokens);

    if (qEnvironmentVariableIsSet("TRANSLATOR_DEBUG_TEMPLATE")) {
        fprintf(stderr, "---- prompt begin (%d tokens) ----\n%s\n---- prompt end ----\n",
                n_tokens, promptStr.c_str());
    }

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

    // 采样链：翻译要的是稳定复现，不是创意
    // 顺序必须是 top_k → top_p → temp → dist，温度放最后才有意义
    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    llama_sampler *smpl = llama_sampler_chain_init(sparams);
    int n_vocab = llama_vocab_n_tokens(vocab);
    // penalties(n_vocab, penalty_last_n, penalty_repeat, penalty_freq, penalty_present)
    // 旧值 1.4 过强，会把正确的字替换成近义字
    // 采样参数按 Hy-MT2 官方推荐（1.8B/7B：temp 0.7 / top_p 0.6 / top_k 20 / penalty 1.05）。
    // 环境变量可覆盖，便于 A/B 对比；dist 用固定种子保证同一输入可复现。
    const float mtTemp = qEnvironmentVariableIsSet("MP_MT_TEMP")
        ? qEnvironmentVariable("MP_MT_TEMP").toFloat() : 0.7f;
    const float mtTopP = qEnvironmentVariableIsSet("MP_MT_TOP_P")
        ? qEnvironmentVariable("MP_MT_TOP_P").toFloat() : 0.6f;
    llama_sampler_chain_add(smpl, llama_sampler_init_penalties(n_vocab, 64, 1.05f, 0.0f, 0.0f));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(20));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(mtTopP, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(mtTemp));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(1234));

    // 生成循环：单句字幕，遇到第一个换行就结束（模型续写的解释都在下一行）
    std::string output;
    int n_cur = n_tokens;
    int max_tokens = 256;

    for (int i = 0; i < max_tokens && !m_cancelled.load(); i++) {
        // 采样
        llama_token new_token = llama_sampler_sample(smpl, m_llamaCtx, -1);

        // 检查结束符：EOS + chat 模型特有的 EOT
        if (llama_vocab_is_eog(vocab, new_token)) {
            break;
        }

        // token 转文本
        char buf[256];
        int len = llama_token_to_piece(vocab, new_token, buf, sizeof(buf), 0, false);
        if (len > 0) {
            output.append(buf, len);
            const QString soFar = QString::fromUtf8(output.c_str());
            if (soFar.contains('\n') && !soFar.trimmed().isEmpty()) {
                break;
            }
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

    QString result = postProcess(QString::fromUtf8(output.c_str()), QString());

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

    // 原文已经是目标语言：直接透传。旧版会把它送去"翻译"，模型原样输出后
    // 又被 remove(原文) 清成空串，字幕直接消失。
    if (looksLikeLanguage(filtered, targetLang)) {
        fprintf(stderr, "[Translator] Skip translate, source already in [%s]: %s\n",
                targetLang.toUtf8().constData(), filtered.toUtf8().constData());
        return filtered;
    }

    fprintf(stderr, "[Translator] Translating [%s] <- [%s]\n",
            targetLang.toUtf8().constData(),
            filtered.toUtf8().constData());

    QString prompt = buildChatPrompt(filtered, targetLang);
    QString result = infer(prompt);

    // 后处理：仅当模型把原文整段复制到译文开头时才剥离
    if (!result.isEmpty() && filtered.length() > 2 &&
        result.startsWith(filtered, Qt::CaseInsensitive)) {
        result = result.mid(filtered.length()).trimmed();
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
