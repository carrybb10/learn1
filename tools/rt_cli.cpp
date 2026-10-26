// rt_cli —— 实时字幕链路的无 GUI 端到端工具
//
// 用法：
//   rt_cli --wav <16k mono pcm16 wav> --asr-model <ggml-small.bin> --vad-model <silero.bin>
//          [--lang 中文|none] [--speed 1.0]
//
// 做的事：把 wav 按实时节奏推进一个 AudioRingBuffer，跑真正的 RealtimeSubtitleWorker
// （VAD 分段 -> whisper 识别 -> 可选翻译），把每条字幕和延迟打印出来。
// 用途：改分段参数/提示词后，不用开视频就能看到"边播边出字"的实际效果。

#include "AudioRingBuffer.h"
#include "RealtimeSubtitleWorker.h"
#include "SileroVad.h"
#include "Translator.h"
#include "WhisperTranscriber.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <vector>

static bool loadWav16k(const QString &path, QVector<float> *out, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *err = QStringLiteral("cannot open wav");
        return false;
    }
    const QByteArray all = f.readAll();
    if (all.size() < 44 || all.left(4) != "RIFF" || all.mid(8, 4) != "WAVE") {
        *err = QStringLiteral("not a RIFF/WAVE file");
        return false;
    }

    int channels = 0, rate = 0, bits = 0;
    int pos = 12;
    while (pos + 8 <= all.size()) {
        const QByteArray id = all.mid(pos, 4);
        const int size = int(uchar(all[pos + 4])) | (int(uchar(all[pos + 5])) << 8)
                       | (int(uchar(all[pos + 6])) << 16) | (int(uchar(all[pos + 7])) << 24);
        pos += 8;
        if (id == "fmt ") {
            channels = int(uchar(all[pos + 2])) | (int(uchar(all[pos + 3])) << 8);
            rate = int(uchar(all[pos + 4])) | (int(uchar(all[pos + 5])) << 8)
                 | (int(uchar(all[pos + 6])) << 16) | (int(uchar(all[pos + 7])) << 24);
            bits = int(uchar(all[pos + 14])) | (int(uchar(all[pos + 15])) << 8);
        } else if (id == "data") {
            if (rate != 16000 || channels != 1 || bits != 16) {
                *err = QStringLiteral("expect 16kHz mono pcm16");
                return false;
            }
            const int n = size / 2;
            out->resize(n);
            const char *p = all.constData() + pos;
            for (int i = 0; i < n; ++i) {
                const qint16 v = qint16(uchar(p[2 * i]) | (int(uchar(p[2 * i + 1])) << 8));
                (*out)[i] = float(v) / 32768.0f;
            }
            return true;
        }
        pos += size + (size & 1);
    }
    *err = QStringLiteral("no data chunk");
    return false;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"wav", "16k mono pcm16 wav", "path"});
    parser.addOption({"asr-model", "whisper ggml model", "path"});
    parser.addOption({"vad-model", "silero vad model", "path"});
    parser.addOption({"lang", "translate target (none = 只识别)", "lang", "none"});
    parser.addOption({"speed", "feed speed multiplier (1 = realtime)", "x", "1.0"});
    parser.process(app);

    const QString wavPath = parser.value("wav");
    const QString asrPath = parser.value("asr-model");
    const QString vadPath = parser.value("vad-model");
    const QString lang = parser.value("lang");
    const double speed = qMax(0.25, parser.value("speed").toDouble());

    QVector<float> pcm;
    QString err;
    if (wavPath.isEmpty() || !loadWav16k(wavPath, &pcm, &err)) {
        fprintf(stderr, "wav error: %s\n", err.toUtf8().constData());
        return 2;
    }
    if (asrPath.isEmpty() || vadPath.isEmpty()) {
        fprintf(stderr, "--asr-model and --vad-model are required\n");
        return 2;
    }

    WhisperTranscriber asr;
    if (!asr.loadModel(asrPath)) {
        fprintf(stderr, "failed to load asr model\n");
        return 3;
    }

    Translator mt;
    bool translate = (lang.compare("none", Qt::CaseInsensitive) != 0);
    // 翻译模型路径由环境变量 MP_TRANSLATOR_MODEL 提供，缺省则只识别不翻译
    const QString mtPath = qEnvironmentVariable("MP_TRANSLATOR_MODEL");
    if (translate && !mtPath.isEmpty() && !mt.loadModel(mtPath)) {
        fprintf(stderr, "translator model load failed, translation disabled\n");
        translate = false;
    } else if (translate && mtPath.isEmpty()) {
        fprintf(stderr, "MP_TRANSLATOR_MODEL not set, translation disabled\n");
        translate = false;
    }

    AudioRingBuffer ring(16000 * 60);
    RealtimeSubtitleWorker worker;
    RealtimeSubtitleWorker::Config cfg;
    cfg.vadModelPath = vadPath;
    cfg.targetLang = translate ? lang : QString();
    cfg.backlogDropSec = 15;
    worker.configure(cfg);
    worker.setSource([&ring]() { return ring.writePosition(); },
                     [&ring](qint64 s, float *out, int n) { return ring.copyFrom(s, out, n); });
    worker.setEngines(&asr, translate ? &mt : nullptr);

    int emitted = 0;
    QObject::connect(&worker, &RealtimeSubtitleWorker::subtitleReady, &app,
                     [&emitted](const QString &text, double t0, double t1, double procMs, double lagSec) {
        ++emitted;
        printf("[%-7.2f - %-7.2f] proc=%5.0fms lag=%5.2fs  %s\n",
               t0, t1, procMs, lagSec, text.toUtf8().constData());
        fflush(stdout);
    });
    QObject::connect(&worker, &RealtimeSubtitleWorker::errorOccurred, &app, [](const QString &e) {
        fprintf(stderr, "worker error: %s\n", e.toUtf8().constData());
    });
    QObject::connect(&worker, &RealtimeSubtitleWorker::statusChanged, &app, [](const QString &s) {
        fprintf(stderr, "[status] %s\n", s.toUtf8().constData());
    });

    QThread thread;
    worker.moveToThread(&thread);
    QObject::connect(&thread, &QThread::started, &worker, &RealtimeSubtitleWorker::start);
    thread.start();

    // 按 speed 倍速把 wav 推进环形缓冲（模拟音频线程写入）
    qint64 fed = 0;
    const int perTick = int(16000 * 0.1 * speed);
    QTimer feeder;
    feeder.setInterval(100);
    QObject::connect(&feeder, &QTimer::timeout, [&]() {
        if (fed >= pcm.size()) {
            feeder.stop();
            // 留出尾巴分段的识别时间
            QTimer::singleShot(3000, &app, &QCoreApplication::quit);
            return;
        }
        const int n = int(qMin<qint64>(perTick, pcm.size() - fed));
        ring.push(pcm.constData() + fed, n);
        fed += n;
    });
    feeder.start();

    app.exec();

    // 关停顺序：先置停止标志（worker 正在识别时也能尽快退出），再让线程结束
    worker.requestStop();
    thread.quit();
    thread.wait();
    printf("emitted %d subtitles\n", emitted);
    return emitted > 0 ? 0 : 1;
}
