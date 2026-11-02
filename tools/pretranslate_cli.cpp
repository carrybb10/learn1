// 预翻译 CLI：给主管线做无 GUI 回归用。
//
// 核心逻辑全在 BatchSubtitlePipeline 里，这里只负责解析参数、把进度打到 stderr。
// 想验证"改了断句/翻译参数后字幕有没有变好"，直接跑这个，不必开播放器。

#include <QCommandLineParser>
#include <QCoreApplication>

#include <cstdio>

#include "BatchSubtitlePipeline.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("pretranslate_cli"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Offline subtitle pipeline: ASR -> cues -> translate -> SRT"));
    parser.addHelpOption();
    parser.addOption({ QStringLiteral("input"), QStringLiteral("video or audio file"), QStringLiteral("path") });
    parser.addOption({ QStringLiteral("out"), QStringLiteral("output .srt path"), QStringLiteral("path") });
    parser.addOption({ QStringLiteral("asr-model"), QStringLiteral("whisper ggml model"), QStringLiteral("path") });
    parser.addOption({ QStringLiteral("vad-model"), QStringLiteral("silero vad model (optional)"), QStringLiteral("path") });
    parser.addOption({ QStringLiteral("asr-lang"), QStringLiteral("source language, default auto"), QStringLiteral("lang"),
                       QStringLiteral("auto") });
    parser.addOption({ QStringLiteral("mt-model"), QStringLiteral("llama gguf translation model"), QStringLiteral("path") });
    parser.addOption({ QStringLiteral("lang"), QStringLiteral("target language, empty = no translation"), QStringLiteral("lang") });
    parser.addOption({ QStringLiteral("max-chars"), QStringLiteral("max chars per cue, default 28"), QStringLiteral("n"),
                       QStringLiteral("28") });
    parser.addOption({ QStringLiteral("cps"), QStringLiteral("chars per second used to estimate cue duration"), QStringLiteral("n"),
                       QStringLiteral("8") });
    parser.addOption({ QStringLiteral("no-cache"), QStringLiteral("ignore cached srt") });
    parser.process(app);

    BatchSubtitlePipeline::Options options;
    options.inputPath = parser.value(QStringLiteral("input"));
    options.srtPath = parser.value(QStringLiteral("out"));
    options.asrModel = parser.value(QStringLiteral("asr-model"));
    options.vadModel = parser.value(QStringLiteral("vad-model"));
    options.asrLanguage = parser.value(QStringLiteral("asr-lang"));
    options.mtModel = parser.value(QStringLiteral("mt-model"));
    options.targetLang = parser.value(QStringLiteral("lang"));
    options.maxChars = qMax(8, parser.value(QStringLiteral("max-chars")).toInt());
    options.cps = qBound(2.0, parser.value(QStringLiteral("cps")).toDouble(), 30.0);
    options.useCache = !parser.isSet(QStringLiteral("no-cache"));

    if (options.inputPath.isEmpty() || options.asrModel.isEmpty()) {
        fprintf(stderr, "--input and --asr-model are required\n");
        return 2;
    }
    if (!options.targetLang.isEmpty() && options.mtModel.isEmpty()) {
        fprintf(stderr, "--lang requires --mt-model\n");
        return 2;
    }

    BatchSubtitlePipeline pipeline;
    const BatchSubtitlePipeline::Result result =
        pipeline.runSync(options, [](const QString &stage, int done, int total) {
            if (total > 0)
                fprintf(stderr, "[%s] %d/%d\n", stage.toUtf8().constData(), done + 1, total);
            else
                fprintf(stderr, "[%s]\n", stage.toUtf8().constData());
        });

    if (!result.ok) {
        fprintf(stderr, "[failed] %s\n", result.error.toUtf8().constData());
        return 1;
    }
    fprintf(stderr, "[done] %s, %d cues, %lld ms%s\n", result.srtPath.toUtf8().constData(), result.cueCount,
            (long long)result.totalMs, result.fromCache ? " (cache)" : "");
    return 0;
}
