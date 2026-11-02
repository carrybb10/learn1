#include "BatchSubtitlePipeline.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <QVector>

#include <cmath>
#include <cstdio>
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

using Cue = BatchSubtitlePipeline::Cue;

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

BatchSubtitlePipeline::BatchSubtitlePipeline(QObject *parent)
    : QObject(parent)
{
}

QString BatchSubtitlePipeline::defaultSrtPath(const Options &options)
{
    if (!options.srtPath.isEmpty())
        return options.srtPath;
    const QFileInfo info(options.inputPath);
    const QString suffix = options.targetLang.isEmpty() ? QStringLiteral("asr") : options.targetLang;
    return info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QLatin1Char('.') + suffix
        + QStringLiteral(".srt");
}

QString BatchSubtitlePipeline::cacheKey(const Options &options)
{
    const QFileInfo input(options.inputPath);
    // 参数版本：改了断句规则或 prompt 就要让它失效，否则用户会看到旧字幕。
    constexpr int kPipelineVersion = 1;
    return QStringList({ QString::number(input.size()), QString::number(input.lastModified().toSecsSinceEpoch()),
                         QFileInfo(options.asrModel).fileName(), QFileInfo(options.vadModel).fileName(),
                         QFileInfo(options.mtModel).fileName(), options.asrLanguage, options.targetLang,
                         QString::number(options.maxChars), QString::number(options.cps, 'f', 1),
                         QString::number(kPipelineVersion) })
        .join(QLatin1Char('|'));
}

BatchSubtitlePipeline::Result BatchSubtitlePipeline::runSync(const Options &options, const ProgressFn &progress)
{
    Result result;
    QElapsedTimer total;
    QElapsedTimer timer;
    total.start();
    timer.start();

    const QString srtPath = defaultSrtPath(options);
    result.srtPath = srtPath;

    // 缓存：同片源、同模型、同参数直接复用，第二次打开是 0 等待。
    const QString keyPath = srtPath + QStringLiteral(".key");
    const QString key = cacheKey(options);
    if (options.useCache && QFileInfo::exists(srtPath)) {
        QFile keyFile(keyPath);
        if (keyFile.open(QIODevice::ReadOnly)) {
            const QString stored = QString::fromUtf8(keyFile.readAll()).trimmed();
            if (stored == key) {
                result.ok = true;
                result.fromCache = true;
                result.totalMs = total.elapsed();
                fprintf(stderr, "[cache] hit: %s\n", srtPath.toUtf8().constData());
                return result;
            }
        }
    }

    if (progress)
        progress(QStringLiteral("decode"), 0, 0);

    QVector<float> samples;
    if (!decodeAudio(options.inputPath, &samples, &result.error))
        return result;
    const double audioSeconds = static_cast<double>(samples.size()) / kSampleRate;
    result.decodeMs = timer.restart();

    WhisperTranscriber asr;
    if (!asr.loadModel(options.asrModel)) {
        result.error = QStringLiteral("cannot load ASR model: %1").arg(options.asrModel);
        return result;
    }

    if (progress)
        progress(QStringLiteral("vad"), 0, 0);

    QVector<QPair<double, double>> windows;
    SileroVad vad;
    VadSegmenter segmenter;
    if (!options.vadModel.isEmpty() && vad.load(options.vadModel)) {
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

    // 合并成 <= 25 秒的识别窗口，减少 whisper 的边界丢词。
    QVector<QPair<double, double>> chunks;
    for (const auto &window : windows) {
        if (chunks.isEmpty() || qMax(chunks.last().second, window.second) - chunks.last().first > 25.0)
            chunks.append(window);
        else
            chunks.last().second = qMax(chunks.last().second, window.second);
    }
    result.vadMs = timer.restart();

    QVector<TranscriptSegment> asrSegments;
    for (int i = 0; i < chunks.size(); ++i) {
        if (progress)
            progress(QStringLiteral("asr"), i, chunks.size());
        const int from = qMax(0, static_cast<int>(chunks[i].first * kSampleRate));
        const int to = qMin(samples.size(), static_cast<int>(chunks[i].second * kSampleRate));
        if (to - from < kSampleRate / 4)  // 短于 250ms 没有识别价值
            continue;
        const QVector<float> chunk = samples.mid(from, to - from);
        const QVector<TranscriptSegment> segments = asr.transcribe(chunk, options.asrLanguage);
        for (TranscriptSegment segment : segments) {
            segment.start += chunks[i].first;
            segment.end += chunks[i].first;
            asrSegments.append(segment);
        }
    }
    result.asrMs = timer.restart();

    QVector<Cue> cues = buildCues(asrSegments, options.maxChars, 0.8, options.cps);
    result.cueCount = cues.size();
    if (cues.isEmpty()) {
        result.error = QStringLiteral("no speech recognized");
        return result;
    }

    if (!options.targetLang.isEmpty()) {
        Translator translator;
        if (!translator.loadModel(options.mtModel)) {
            result.error = QStringLiteral("cannot load translation model: %1").arg(options.mtModel);
            return result;
        }
        QVector<TranslationContextTurn> context;
        for (int i = 0; i < cues.size(); ++i) {
            if (progress)
                progress(QStringLiteral("translate"), i, cues.size());
            cues[i].translated = translator.translateWithContext(cues[i].text, options.targetLang, context,
                                                                 options.asrLanguage);
            cues[i].translated.replace(QStringLiteral("，。"), QStringLiteral("。"));
            cues[i].translated.replace(QStringLiteral("，，"), QStringLiteral("，"));
            if (!cues[i].translated.isEmpty() && cues[i].translated != cues[i].text) {
                context.append({ cues[i].text, cues[i].translated });
                while (context.size() > 2)
                    context.removeFirst();
            }
        }
        result.mtMs = timer.restart();
    }

    if (progress)
        progress(QStringLiteral("write"), 0, 0);

    const bool translated = !options.targetLang.isEmpty();
    if (!writeSrt(srtPath, cues, translated, &result.error))
        return result;

    // 只有整篇成功才写缓存键，避免半成品被当成命中。
    QFile keyFile(keyPath);
    if (keyFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        keyFile.write(key.toUtf8());
        keyFile.close();
    }

    result.ok = true;
    result.totalMs = total.elapsed();
    fprintf(stderr, "[pipeline] %d cues, total %lld ms (decode %lld + vad %lld + asr %lld + mt %lld)\n",
            result.cueCount, (long long)result.totalMs, (long long)result.decodeMs, (long long)result.vadMs,
            (long long)result.asrMs, (long long)result.mtMs);
    return result;
}
