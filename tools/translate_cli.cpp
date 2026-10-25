// 无 GUI 的翻译回归测试工具（迭代翻译质量 / 验证 prompt 改动）
// 用法：translate_cli --model <gguf> --lang 中文 --input sentences.txt [--repeat N]
// 输入文件 UTF-8，一行一句；输出 "序号\t耗时ms\t原文\t译文"，便于 diff 前后结果。

#include "Translator.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <cstdio>

static void emitLine(const QString &line)
{
    QByteArray utf8 = line.toUtf8();
    fwrite(utf8.constData(), 1, utf8.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("model"), QStringLiteral("gguf 模型路径"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("lang"), QStringLiteral("目标语言，如 中文/英语/日语/韩语"), QStringLiteral("lang"), QStringLiteral("中文")});
    parser.addOption({QStringLiteral("input"), QStringLiteral("输入文件（UTF-8，一行一句），省略则读 stdin"), QStringLiteral("file")});
    parser.process(app);

    const QString modelPath = parser.value(QStringLiteral("model"));
    const QString targetLang = parser.value(QStringLiteral("lang"));
    if (modelPath.isEmpty()) {
        fprintf(stderr, "--model is required\n");
        return 2;
    }

    QStringList sentences;
    const QString inputPath = parser.value(QStringLiteral("input"));
    if (inputPath.isEmpty()) {
        QTextStream in(stdin);
        in.setEncoding(QStringConverter::Utf8);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (!line.isEmpty()) sentences.append(line);
        }
    } else {
        QFile file(inputPath);
        if (!file.open(QIODevice::ReadOnly)) {
            fprintf(stderr, "cannot open input file\n");
            return 2;
        }
        QTextStream in(&file);
        in.setEncoding(QStringConverter::Utf8);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (!line.isEmpty()) sentences.append(line);
        }
    }

    if (sentences.isEmpty()) {
        fprintf(stderr, "no input sentences\n");
        return 2;
    }

    Translator translator;
    QObject::connect(&translator, &Translator::modelLoaded,
                     [](bool ok, const QString &error) {
                         fprintf(stderr, "[model] %s %s\n", ok ? "loaded" : "failed",
                                 error.toUtf8().constData());
                     });

    QElapsedTimer timer;
    timer.start();
    if (!translator.loadModel(modelPath)) {
        return 1;
    }
    fprintf(stderr, "[model] load %lld ms\n", (long long)timer.elapsed());

    int failed = 0;
    for (int i = 0; i < sentences.size(); ++i) {
        timer.restart();
        const QString out = translator.translate(sentences[i], targetLang);
        const qint64 ms = timer.elapsed();
        if (out.isEmpty()) ++failed;
        emitLine(QStringLiteral("%1\t%2\t%3\t%4")
                     .arg(i + 1).arg(ms).arg(sentences[i], out));
    }

    fprintf(stderr, "[done] %d sentences, %d empty\n", (int)sentences.size(), failed);
    return failed == sentences.size() ? 1 : 0;
}
