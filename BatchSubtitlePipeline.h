#pragma once

#include <QObject>
#include <QString>

#include <functional>

// 离线字幕流水线：解码 -> VAD 分段 -> whisper 识别 -> 断句 -> 批量翻译 -> SRT。
//
// 与实时字幕（RealtimeSubtitleWorker）共用 SileroVad / VadSegmenter / WhisperTranscriber /
// Translator，区别只有两点：整篇一次处理，因此翻译能带上文；时间轴直接用 whisper 的
// 段落时间，不需要和播放时钟对齐。
class BatchSubtitlePipeline : public QObject
{
    Q_OBJECT

public:
    struct Cue
    {
        double start = 0.0;
        double end = 0.0;
        QString text;
        QString translated;
    };

    struct Options
    {
        // 复用调用方已经加载好的引擎。同时存在两份 GPU 模型会把 8GB 显存挤爆，
        // GUI 里必须传这两项；单独跑 CLI 时为 nullptr，由 pipeline 自己加载。
        class WhisperTranscriber *asrEngine = nullptr;
        class Translator *mtEngine = nullptr;

        QString inputPath;
        QString srtPath;                      // 空则用 defaultSrtPath() 派生
        QString asrModel;
        QString vadModel;                     // 空则按 25 秒硬切
        QString asrLanguage = QStringLiteral("auto");
        QString mtModel;
        QString targetLang;                   // 空 = 只生成原文 SRT
        int maxChars = 28;
        double cps = 8.0;                     // 每秒字数，用来估算每条字幕时长
        bool useCache = true;
        int subtitleOffsetMs = 0;              // 整体平移字幕时间，用来对上人耳听到的位置
    };

    struct Result
    {
        bool ok = false;
        bool fromCache = false;
        QString srtPath;
        int cueCount = 0;
        qint64 decodeMs = 0;
        qint64 vadMs = 0;
        qint64 asrMs = 0;
        qint64 mtMs = 0;
        qint64 totalMs = 0;
        QString error;
    };

    // stage = decode / vad / asr / translate / write；done/total 在 asr 与 translate 阶段有效。
    using ProgressFn = std::function<void(const QString &stage, int done, int total)>;

    explicit BatchSubtitlePipeline(QObject *parent = nullptr);

    // 阻塞执行。GUI 里请放到 QThread 上调用，不要在 UI 线程里直接跑。
    Result runSync(const Options &options, const ProgressFn &progress = ProgressFn());

    // 同目录、同名的 <basename>.<lang>.srt；没有目标语言时用 .asr.srt。
    static QString defaultSrtPath(const Options &options);

    // 缓存键：文件大小 + 修改时间 + 模型 + 参数。任何一项变了都要重跑。
    static QString cacheKey(const Options &options);
};
