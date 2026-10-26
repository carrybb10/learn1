// vad_cli：不开 GUI、不用视频文件，验证"音频 → Silero VAD → 语音分段"这条链路。
//
// 用法：
//   vad_cli.exe --model <silero ggml> --wav <16k mono pcm wav> [--json]
//
// 输出每段语音的起止毫秒。改 VAD 参数或升级 whisper.cpp 后跑一次即可确认分段没坏。

#include "SileroVad.h"
#include "VadSegmenter.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QVector>

#include <cstdio>
#include <cstring>

namespace {

bool readWav16kMono(const QString &path, QVector<float> *samples)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QByteArray blob = file.readAll();
    if (blob.size() < 44 || std::memcmp(blob.constData(), "RIFF", 4) != 0
        || std::memcmp(blob.constData() + 8, "WAVE", 4) != 0) {
        std::fprintf(stderr, "not a RIFF/WAVE file: %s\n", qPrintable(path));
        return false;
    }

    int channels = 0;
    int sampleRate = 0;
    int bitsPerSample = 0;
    int dataOffset = -1;
    int dataSize = 0;

    int offset = 12;
    while (offset + 8 <= blob.size()) {
        const char *id = blob.constData() + offset;
        int chunkSize = 0;
        std::memcpy(&chunkSize, blob.constData() + offset + 4, 4);
        const int body = offset + 8;

        if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= blob.size()) {
            short format = 0;
            std::memcpy(&format, blob.constData() + body, 2);
            std::memcpy(&channels, blob.constData() + body + 2, 2);
            std::memcpy(&sampleRate, blob.constData() + body + 4, 4);
            short bits = 0;
            std::memcpy(&bits, blob.constData() + body + 14, 2);
            bitsPerSample = bits;
            if (format != 1) {
                std::fprintf(stderr, "only PCM wav is supported (format=%d)\n", format);
                return false;
            }
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataOffset = body;
            dataSize = qMin(chunkSize, blob.size() - body);
            break;
        }

        if (chunkSize < 0)
            break;
        offset = body + chunkSize + (chunkSize & 1);
    }

    if (dataOffset < 0 || channels != 1 || sampleRate != 16000 || bitsPerSample != 16) {
        std::fprintf(stderr,
                     "need 16k mono 16-bit wav, got rate=%d channels=%d bits=%d\n",
                     sampleRate, channels, bitsPerSample);
        return false;
    }

    const int count = dataSize / 2;
    samples->resize(count);
    const char *p = blob.constData() + dataOffset;
    for (int i = 0; i < count; ++i) {
        short v = 0;
        std::memcpy(&v, p + i * 2, 2);
        (*samples)[i] = float(v) / 32768.0f;
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("vad_cli");

    QCommandLineParser parser;
    parser.setApplicationDescription("Silero VAD streaming segment check");
    parser.addHelpOption();
    QCommandLineOption modelOpt("model", "Path to silero vad ggml model.", "path");
    QCommandLineOption wavOpt("wav", "Path to 16k mono 16-bit wav.", "path");
    QCommandLineOption jsonOpt("json", "Print segments as json.");
    QCommandLineOption wholeOpt("whole", "Use one-shot VAD pass (reference) instead of streaming.");
    QCommandLineOption chunkOpt("chunk-ms", "Streaming chunk size in ms (default 192).", "ms", "192");
    parser.addOption(modelOpt);
    parser.addOption(wavOpt);
    parser.addOption(jsonOpt);
    parser.addOption(wholeOpt);
    parser.addOption(chunkOpt);
    parser.process(app);

    if (!parser.isSet(modelOpt) || !parser.isSet(wavOpt)) {
        parser.showHelp(2);
    }

    QVector<float> samples;
    if (!readWav16kMono(parser.value(wavOpt), &samples)) {
        std::fprintf(stderr, "failed to load wav: %s\n", qPrintable(parser.value(wavOpt)));
        return 2;
    }

    SileroVad vad;
    if (!vad.load(parser.value(modelOpt))) {
        std::fprintf(stderr, "failed to load vad model: %s\n", qPrintable(parser.value(modelOpt)));
        return 2;
    }

    // 默认 192ms 一批（6 窗口）；chunk 越大，跨调用丢掉 LSTM 状态的影响越小
    int chunkSamples = int(16000.0 * parser.value(chunkOpt).toInt() / 1000);
    chunkSamples = qMax(SileroVad::kWindowSamples, (chunkSamples / SileroVad::kWindowSamples) * SileroVad::kWindowSamples);

    QVector<VadSegmenter::Segment> segments;
    VadSegmenter segmenter;

    int consumed = 0;
    int windows = 0;
    int speechWindows = 0;
    QVector<float> probs;
    probs.reserve(chunkSamples / SileroVad::kWindowSamples);

    if (parser.isSet(wholeOpt)) {
        // 参考实现：整段一次推理，用来确认流式结果没跑偏
        if (!vad.detectWhole(samples.constData(), samples.size(), &probs)) {
            std::fprintf(stderr, "whole-file vad detect failed\n");
            return 3;
        }
        windows = probs.size();
        std::printf("[info] whole-file pass: %d probs\n", windows);
        for (float p : probs) {
            if (p >= segmenter.params().threshold)
                ++speechWindows;
            segmenter.push(p, &segments);
        }
        consumed = samples.size();
    }

    if (!parser.isSet(wholeOpt)) {
        // 流式入口必须先把 LSTM 状态清零一次：新分配的 backend buffer 并不保证是 0，
        // 带着脏状态推理会系统性低估语音概率（实测 62.8% -> 21.5%）。
        vad.resetState();
    }

    while (consumed < samples.size()) {
        const int n = qMin(chunkSamples, samples.size() - consumed);        probs.clear();
        if (!vad.detect(samples.constData() + consumed, n, &probs)) {
            std::fprintf(stderr, "vad detect failed at sample %d\n", consumed);
            return 3;
        }

        if (windows == 0) {
            const int expected = (n + SileroVad::kWindowSamples - 1) / SileroVad::kWindowSamples;
            std::printf("[info] %d samples -> %d probs (expected %d)\n", n, probs.size(), expected);
            if (probs.size() != expected)
                std::printf("[warn] prob count mismatch, window may not be %d\n", SileroVad::kWindowSamples);
        }

        for (float p : probs) {
            ++windows;
            if (p >= segmenter.params().threshold)
                ++speechWindows;
            segmenter.push(p, &segments);
        }

        consumed += n;
    }
    segmenter.flush(&segments);

    const double msPerWindow = 1000.0 * SileroVad::kWindowSamples / 16000.0;

    if (parser.isSet(jsonOpt)) {
        std::printf("{\"duration_ms\": %lld, \"windows\": %d, \"speech_ratio\": %.3f, \"segments\": [",
                    static_cast<long long>(samples.size() * 1000LL / 16000), windows,
                    windows > 0 ? double(speechWindows) / windows : 0.0);
        for (int i = 0; i < segments.size(); ++i) {
            std::printf("%s{\"t0\": %lld, \"t1\": %lld}",
                        i ? ", " : "",
                        static_cast<long long>(segments[i].t0Frame * msPerWindow),
                        static_cast<long long>(segments[i].t1Frame * msPerWindow));
        }
        std::printf("]}\n");
    } else {
        std::printf("[info] audio %lld ms, %d windows, speech ratio %.1f%%\n",
                    static_cast<long long>(samples.size() * 1000LL / 16000), windows,
                    windows > 0 ? 100.0 * speechWindows / windows : 0.0);
        std::printf("segments: %d\n", segments.size());
        for (const auto &s : segments) {
            std::printf("  [%6lld - %6lld) ms  %5lld ms\n",
                        static_cast<long long>(s.t0Frame * msPerWindow),
                        static_cast<long long>(s.t1Frame * msPerWindow),
                        static_cast<long long>((s.t1Frame - s.t0Frame) * msPerWindow));
        }
    }

    return segments.isEmpty() ? 1 : 0;
}
