// 预翻译原型：视频/音频 -> 整篇 ASR -> 断句成字幕条目 -> 批量翻译 -> SRT
//
// 目的只有一个：用一条真实片源回答"离线字幕流水线值不值得做"。
// 因此这里刻意不接 GUI、不做缓存、不做并发，只把链路跑通并把每一段耗时打出来。
// 验证通过后，核心逻辑（decodeAudio / buildCues）会抽到 BatchSubtitlePipeline 里复用。

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <QVector>

#include <cstdio>
#include <cmath>
#include <numeric>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include "SileroVad.h"
#include "Translator.h"
#include "VadSegmenter.h"
#include "WhisperTranscriber.h"

namespace {

constexpr int kSampleRate = 16000;

struct Cue
{
    double start = 0.0;
    double end = 0.0;
    QString text;
    QString translated;
};

// 用 FFmpeg 把任意文件的第一条音轨解码成 16kHz 单声道 float。
// 原型阶段不做音轨选择：真实片源有多音轨时，后续加 --audio-index。
bool decodeAudio(const QString &path, QVector<float> *out, QString *error)
{
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        *error = QStringLiteral("cannot open input");
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        *error = QStringLiteral("cannot read stream info");
        avformat_close_input(&fmt);
        return false;
    }

    const int streamIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        *error = QStringLiteral("no audio stream");
        avformat_close_input(&fmt);
        return false;
    }

    AVStream *stream = fmt->streams[streamIndex];
    fprintf(stderr, "[decode] stream %d: codec_id=%d sample_rate=%d\n", streamIndex,
            (int)stream->codecpar->codec_id, stream->codecpar->sample_rate);
    fflush(stderr);
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        *error = QStringLiteral("unsupported audio codec");
        avformat_close_input(&fmt);
        return false;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, stream->codecpar);
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        *error = QStringLiteral("cannot open decoder");
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }

    AVChannelLayout inLayout{};
    av_channel_layout_copy(&inLayout, &ctx->ch_layout);
    SwrContext *swr = nullptr;
    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_MONO;
    const int swrRet = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, kSampleRate,
                                           &inLayout, ctx->sample_fmt, ctx->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&inLayout);
    if (swrRet < 0 || !swr || swr_init(swr) < 0) {
        *error = QStringLiteral("cannot init resampler");
        swr_free(&swr);
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }

    fprintf(stderr, "[decode] decoder opened, sample_fmt=%d\n", (int)ctx->sample_fmt);
    fflush(stderr);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    QVector<float> buffer(4096);
    out->clear();

    auto drain = [&](AVFrame *f) {
        int64_t delay = swr_get_delay(swr, ctx->sample_rate);
        const int capacity = static_cast<int>(av_rescale_rnd(delay + f->nb_samples, kSampleRate, ctx->sample_rate, AV_ROUND_UP));
        if (capacity <= 0)
            return;
        if (buffer.size() < capacity)
            buffer.resize(capacity);
        uint8_t *dst[1] = { reinterpret_cast<uint8_t *>(buffer.data()) };
        const int produced = swr_convert(swr, dst, capacity,
                                         const_cast<const uint8_t **>(f->extended_data), f->nb_samples);
        for (int i = 0; i < produced; ++i)
            out->append(buffer[i]);
    };

    int packets = 0;
    while (av_read_frame(fmt, packet) >= 0) {
        ++packets;
        if (packet->stream_index == streamIndex) {
            if (avcodec_send_packet(ctx, packet) == 0) {
                while (avcodec_receive_frame(ctx, frame) == 0)
                    drain(frame);
            }
        }
        av_packet_unref(packet);
    }
    fprintf(stderr, "[decode] read %d packets, decoded %d samples\n", packets, (int)out->size());
    fflush(stderr);
    avcodec_send_packet(ctx, nullptr);
    while (avcodec_receive_frame(ctx, frame) == 0)
        drain(frame);
    fprintf(stderr, "[decode] flush done, %d samples\n", (int)out->size());
    fflush(stderr);

    // 冲掉重采样器里残留的尾巴，否则最后几十毫秒会丢
    int64_t delay = swr_get_delay(swr, ctx->sample_rate);
    if (delay > 0) {
        const int capacity = static_cast<int>(av_rescale_rnd(delay, kSampleRate, ctx->sample_rate, AV_ROUND_UP));
        if (buffer.size() < capacity)
            buffer.resize(capacity);
        uint8_t *dst[1] = { reinterpret_cast<uint8_t *>(buffer.data()) };
        const int produced = swr_convert(swr, dst, capacity, nullptr, 0);
        for (int i = 0; i < produced; ++i)
            out->append(buffer[i]);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return !out->isEmpty();
}

// 断句规则（后续要抽成纯函数并单测）：
// 1. 先按句末标点切句，标点跟着前一句；
// 2. 一句超过 maxChars 就在逗号/顿号处再切；
// 3. 还是超长就硬切；
// 4. 时间按字数比例分配，过短的条目并到前一条（避免 0.2 秒闪一下）。
QVector<Cue> buildCues(const QVector<TranscriptSegment> &segments, int maxChars, double minDuration, double charsPerSecond)
{
    static const QRegularExpression sentenceEnd(QStringLiteral("[。！？!?…]+"));
    static const QRegularExpression clauseEnd(QStringLiteral("[，、；：,;:]+"));
    QVector<Cue> cues;

    auto splitKeepDelimiter = [](const QString &text, const QRegularExpression &re) {
        QStringList parts;
        int start = 0;
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            const int end = m.capturedEnd();
            parts.append(text.mid(start, end - start));
            start = end;
        }
        if (start < text.size())
            parts.append(text.mid(start));
        return parts;
    };

    // whisper 输出的是"呼吸段"，不是句子：间隔小且不超长的相邻段先拼起来
    QVector<TranscriptSegment> merged;
    for (const TranscriptSegment &segment : segments) {
        const QString text = segment.text.trimmed();
        if (text.isEmpty())
            continue;
        if (!merged.isEmpty() && segment.start - merged.last().end <= 0.5
            && merged.last().text.size() + text.size() <= maxChars) {
            merged.last().text += text;
            merged.last().end = segment.end;
            continue;
        }
        merged.append({ segment.start, segment.end, text });
    }

    for (const TranscriptSegment &segment : merged) {
        QString text = segment.text.trimmed();
        if (text.isEmpty())
            continue;

        // 步骤 1+2+3：切成不超过 maxChars 的片段
        QStringList pieces;
        const QStringList sentences = splitKeepDelimiter(text, sentenceEnd);
        for (const QString &sentence : sentences) {
            if (sentence.size() <= maxChars) {
                pieces.append(sentence);
                continue;
            }
            for (const QString &clause : splitKeepDelimiter(sentence, clauseEnd)) {
                if (clause.size() <= maxChars) {
                    pieces.append(clause);
                    continue;
                }
                for (int i = 0; i < clause.size(); i += maxChars)
                    pieces.append(clause.mid(i, maxChars));
            }
        }

        // 步骤 4：按字数比例分配时间
        int totalChars = 0;
        for (const QString &piece : pieces)
            totalChars += piece.size();
        if (totalChars == 0)
            continue;

        // 每条时长由字数按语速估算，段首对齐；累计不超过段尾，剩余时间自然成为句间间隙。
        const double segmentEnd = qMax(0.0, segment.end);
        const double maxCueDuration = 8.0;
        double cursor = segment.start;
        for (const QString &piece : pieces) {
            const double estimated = (static_cast<double>(piece.trimmed().size()) / charsPerSecond);
            const double duration = qBound(minDuration, estimated, maxCueDuration);
            Cue cue;
            cue.start = cursor;
            cue.end = qMin(cursor + duration, segmentEnd);
            cue.text = piece.trimmed();
            cursor = cue.end;
            if (cue.text.isEmpty())
                continue;
            if (!cues.isEmpty() && cue.end - cue.start < minDuration && cues.last().end >= cue.start - 1e-3) {
                cues.last().text += cue.text;
                cues.last().end = cue.end;
                continue;
            }
            cues.append(cue);
        }
    }
    return cues;
}

QString formatSrtTime(double seconds)
{
    if (seconds < 0)
        seconds = 0;
    const int totalMs = static_cast<int>(std::llround(seconds * 1000.0));
    const int ms = totalMs % 1000;
    const int totalSeconds = totalMs / 1000;
    const int s = totalSeconds % 60;
    const int m = (totalSeconds / 60) % 60;
    const int h = totalSeconds / 3600;
    return QStringLiteral("%1:%2:%3,%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

bool writeSrt(const QString &path, const QVector<Cue> &cues, bool translated, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out.setGenerateByteOrderMark(true);  // Windows 播放器对无 BOM 的 UTF-8 支持不一致
    int index = 1;
    for (const Cue &cue : cues) {
        const QString body = translated ? cue.translated : cue.text;
        if (body.trimmed().isEmpty())
            continue;
        out << index++ << '\n'
            << formatSrtTime(cue.start) << " --> " << formatSrtTime(cue.end) << '\n'
            << body.trimmed() << "\n\n";
    }
    out.flush();
    file.close();
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("pretranslate_cli"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Offline subtitle pipeline prototype (ASR -> cues -> translate -> SRT)"));
    parser.addHelpOption();
    parser.addOption({ QStringLiteral("input"), QStringLiteral("video or audio file"), QStringLiteral("path") });
    parser.addOption({ QStringLiteral("out"), QStringLiteral("output .srt path"), QStringLiteral("path") });
    parser.addOption({ QStringLiteral("asr-model"), QStringLiteral("whisper ggml model"), QStringLiteral("path") });
    parser.addOption({ QStringLiteral("vad-model"), QStringLiteral("silero vad model (optional)"), QStringLiteral("path") });
    parser.addOption({ QStringLiteral("asr-lang"), QStringLiteral("source language, default auto"), QStringLiteral("lang"), QStringLiteral("auto") });
    parser.addOption({ QStringLiteral("mt-model"), QStringLiteral("llama gguf translation model"), QStringLiteral("path") });
    parser.addOption({ QStringLiteral("lang"), QStringLiteral("target language, empty = no translation"), QStringLiteral("lang") });
    parser.addOption({ QStringLiteral("max-chars"), QStringLiteral("max chars per cue, default 28"), QStringLiteral("n"), QStringLiteral("28") });
    parser.addOption({ QStringLiteral("cps"), QStringLiteral("chars per second used to estimate cue duration, default 8"), QStringLiteral("n"), QStringLiteral("8") });
    parser.process(app);

    const QString inputPath = parser.value(QStringLiteral("input"));
    const QString outPath = parser.value(QStringLiteral("out"));
    const QString asrModel = parser.value(QStringLiteral("asr-model"));
    const QString vadModel = parser.value(QStringLiteral("vad-model"));
    const QString sourceLang = parser.value(QStringLiteral("asr-lang"));
    const QString mtModel = parser.value(QStringLiteral("mt-model"));
    const QString targetLang = parser.value(QStringLiteral("lang"));
    if (inputPath.isEmpty() || outPath.isEmpty() || asrModel.isEmpty()) {
        fprintf(stderr, "--input, --out and --asr-model are required\n");
        return 2;
    }
    if (!targetLang.isEmpty() && mtModel.isEmpty()) {
        fprintf(stderr, "--lang requires --mt-model\n");
        return 2;
    }
    const int maxChars = qMax(8, parser.value(QStringLiteral("max-chars")).toInt());
    const double charsPerSecond = qBound(2.0, parser.value(QStringLiteral("cps")).toDouble(), 30.0);

    QElapsedTimer timer;
    QElapsedTimer total;
    timer.start();
    total.start();

    // 1. 解码
    QVector<float> samples;
    QString error;
    fprintf(stderr, "[decode] input: %s\n", inputPath.toUtf8().constData());
    fflush(stderr);
    if (!decodeAudio(inputPath, &samples, &error)) {
        fprintf(stderr, "[decode] failed: %s\n", error.toUtf8().constData());
        return 1;
    }
    const double audioSeconds = static_cast<double>(samples.size()) / kSampleRate;
    const qint64 decodeMs = timer.restart();
    fprintf(stderr, "[decode] %.1f s audio in %lld ms\n", audioSeconds, (long long)decodeMs);

    // 2. 语音分段（可选）：没有 VAD 就按 25 秒硬切，先保证能跑
    QVector<TranscriptSegment> asrSegments;
    WhisperTranscriber asr;
    if (!asr.loadModel(asrModel)) {
        fprintf(stderr, "[asr] model load failed\n");
        return 1;
    }
    const qint64 loadMs = timer.restart();
    fprintf(stderr, "[asr] model loaded in %lld ms\n", (long long)loadMs);

    QVector<QPair<double, double>> windows;  // 音频时间轴上的识别窗口
    SileroVad vad;
    VadSegmenter segmenter;
    if (!vadModel.isEmpty() && vad.load(vadModel)) {
        for (int offset = 0; offset < samples.size(); offset += 512 * 6) {
            const int count = qMin(512 * 6, samples.size() - offset);
            QVector<float> probabilities;
            if (!vad.detect(samples.constData() + offset, count, &probabilities))
                continue;
            QVector<VadSegmenter::Segment> segments;
            for (float probability : probabilities)
                segmenter.push(probability, &segments);
            for (const auto &segment : segments) {
                const double start = static_cast<double>(segment.t0Frame) * 512 / kSampleRate;
                const double end = static_cast<double>(segment.t1Frame) * 512 / kSampleRate;
                if (windows.isEmpty() || start - windows.last().second > 0.6)
                    windows.append({ start, end });
                else
                    windows.last().second = end;
            }
        }
        QVector<VadSegmenter::Segment> tail;
        segmenter.flush(&tail);
        for (const auto &segment : tail) {
            const double start = static_cast<double>(segment.t0Frame) * 512 / kSampleRate;
            const double end = static_cast<double>(segment.t1Frame) * 512 / kSampleRate;
            if (windows.isEmpty() || start - windows.last().second > 0.6)
                windows.append({ start, end });
            else
                windows.last().second = end;
        }
        if (windows.isEmpty())
            fprintf(stderr, "[vad] no speech detected, falling back to fixed windows\n");
    }
    if (windows.isEmpty()) {
        for (double start = 0; start < audioSeconds; start += 25.0)
            windows.append({ start, qMin(audioSeconds, start + 25.0) });
    }
    // 合并成 <= 25 秒的识别窗口，减少 whisper 的边界丢词
    QVector<QPair<double, double>> chunks;
    for (const auto &window : windows) {
        if (chunks.isEmpty() || qMax(chunks.last().second, window.second) - chunks.last().first > 25.0)
            chunks.append(window);
        else
            chunks.last().second = qMax(chunks.last().second, window.second);
    }
    const qint64 vadMs = timer.restart();
    fprintf(stderr, "[vad] %d speech windows -> %d asr chunks in %lld ms\n", (int)windows.size(), (int)chunks.size(), (long long)vadMs);

    // 3. 逐块识别，时间戳加回块偏移
    for (int i = 0; i < chunks.size(); ++i) {
        const int from = qMax(0, static_cast<int>(chunks[i].first * kSampleRate));
        const int to = qMin(samples.size(), static_cast<int>(chunks[i].second * kSampleRate));
        if (to - from < kSampleRate / 4)  // 短于 250ms 的块没有识别价值
            continue;
        const QVector<float> chunk = samples.mid(from, to - from);
        const QVector<TranscriptSegment> result = asr.transcribe(chunk, sourceLang);
        for (TranscriptSegment segment : result) {
            segment.start += chunks[i].first;
            segment.end += chunks[i].first;
            asrSegments.append(segment);
        }
        fprintf(stderr, "[asr] chunk %d/%d: %d segments\n", i + 1, (int)chunks.size(), (int)result.size());
    }
    const qint64 asrMs = timer.restart();
    fprintf(stderr, "[asr] %d segments in %lld ms (%.2fx realtime)\n", (int)asrSegments.size(),
            (long long)asrMs, audioSeconds > 0 ? (asrMs / 1000.0) / audioSeconds : 0.0);

    // 4. 断句成字幕条目
    const QVector<Cue> rawCues = buildCues(asrSegments, maxChars, 0.8, charsPerSecond);
    QVector<Cue> cues = rawCues;
    fprintf(stderr, "[cues] %d cues, avg %.1f chars\n", (int)cues.size(),
            cues.isEmpty() ? 0.0 : (std::accumulate(cues.begin(), cues.end(), 0.0,
                                                    [](double acc, const Cue &cue) { return acc + cue.text.size(); }) / cues.size()));

    // 5. 批量翻译（带最近 2 条上下文）
    qint64 mtMs = 0;
    if (!targetLang.isEmpty()) {
        Translator translator;
        if (!translator.loadModel(mtModel)) {
            fprintf(stderr, "[mt] model load failed\n");
            return 1;
        }
        QVector<TranslationContextTurn> context;
        for (int i = 0; i < cues.size(); ++i) {
            cues[i].translated = translator.translateWithContext(cues[i].text, targetLang, context, sourceLang);
            cues[i].translated.replace(QStringLiteral("，。"), QStringLiteral("。"));
            cues[i].translated.replace(QStringLiteral("，，"), QStringLiteral("，"));
            if (!cues[i].translated.isEmpty() && cues[i].translated != cues[i].text) {
                context.append({ cues[i].text, cues[i].translated });
                while (context.size() > 2)
                    context.removeFirst();
            }
            fprintf(stderr, "[mt] %d/%d %s\n", i + 1, (int)cues.size(), cues[i].translated.toUtf8().constData());
        }
        mtMs = timer.restart();
        fprintf(stderr, "[mt] %d cues in %lld ms\n", (int)cues.size(), (long long)mtMs);
    }

    // 6. 落盘
    const bool translated = !targetLang.isEmpty();
    if (!writeSrt(outPath, cues, translated, &error)) {
        fprintf(stderr, "[srt] failed: %s\n", error.toUtf8().constData());
        return 1;
    }
    fprintf(stderr, "[done] %s, total %lld ms (decode %lld + asr %lld + mt %lld)\n", outPath.toUtf8().constData(),
            (long long)total.elapsed(), (long long)decodeMs, (long long)asrMs, (long long)mtMs);
    return 0;
}
