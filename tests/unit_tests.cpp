// 纯函数单元测试：不加载模型、不开窗口、不碰 D3D11。
// 覆盖翻译后处理（历史上出过"输出第二语言行/同语言被吞"的坑）与模型路径解析。
// 运行：cmake --build build --config Debug --target unit_tests && ctest --test-dir build -C Debug

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <cstdio>

#include "AppPaths.h"
#include "Translator.h"

static int g_failed = 0;
static int g_total = 0;

static void report(bool ok, const char *expr, const QString &detail)
{
    ++g_total;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL %s  %s\n", expr, detail.toUtf8().constData());
    }
}

#define CHECK(cond) report((cond), #cond, QString())

#define CHECK_EQ(actual, expected)                                                        \
    do {                                                                                  \
        const QString a_ = (actual);                                                      \
        const QString e_ = (expected);                                                    \
        report(a_ == e_, #actual " == " #expected,                                        \
               QString("got [%1] want [%2]").arg(a_, e_));                                \
    } while (0)

static void testLooksLikeLanguage()
{
    // 中文：CJK 占比高
    CHECK(Translator::looksLikeLanguage(QString::fromUtf8("我们必须在午夜之前离开这座城市。"), "中文"));
    CHECK(!Translator::looksLikeLanguage(QString::fromUtf8("I never thought we'd make it this far."), "中文"));
    // 日语必须含假名，纯汉字句不算日语
    CHECK(Translator::looksLikeLanguage(QString::fromUtf8("こんなところで会うとは思わなかった。"), "日语"));
    CHECK(!Translator::looksLikeLanguage(QString::fromUtf8("这个房间很安静。"), "日语"));
    // 韩语看谚文
    CHECK(Translator::looksLikeLanguage(QString::fromUtf8("이게 마지막 기회야."), "韩语"));
    CHECK(!Translator::looksLikeLanguage(QString::fromUtf8("The signal is getting weaker."), "韩语"));
    // 英语靠高频虚词区分，法语不能被当成英语透传
    CHECK(Translator::looksLikeLanguage(QString::fromUtf8("She has been waiting for you at the station."), "英语"));
    CHECK(!Translator::looksLikeLanguage(QString::fromUtf8("Nous n'avons plus beaucoup de temps."), "英语"));
    // 空文本不当作任何语言
    CHECK(!Translator::looksLikeLanguage(QString(), "中文"));
}

static void testPostProcess()
{
    // 模型多吐一行 "**Translation:** xxx" 时只保留译文
    CHECK_EQ(Translator::postProcess(QString::fromUtf8("**Translation:** hello there"), "中文"),
             QString::fromUtf8("hello there"));
    // 前缀标签 + 包裹引号
    CHECK_EQ(Translator::postProcess(QString::fromUtf8("译文：「你好」"), "中文"),
             QString::fromUtf8("你好"));
    // 只取第一条有效行（后面是解释性文字）
    CHECK_EQ(Translator::postProcess(QString::fromUtf8("你好。\n以上是本次翻译结果。"), "中文"),
             QString::fromUtf8("你好。"));
    // chat 结束标记被截断
    CHECK_EQ(Translator::postProcess(QString::fromUtf8("再见<|im_end|>"), "中文"),
             QString::fromUtf8("再见"));
    // 单引号包裹
    CHECK_EQ(Translator::postProcess(QString::fromUtf8("\"Goodbye\""), "中文"),
             QString::fromUtf8("Goodbye"));
    // 空输入不崩
    CHECK_EQ(Translator::postProcess(QString(), "中文"), QString());
}

static void testBuildPrompt()
{
    const QString prompt = Translator::buildPrompt(QString::fromUtf8("Hello."), QString::fromUtf8("中文"));
    CHECK(prompt.contains(QString::fromUtf8("Hello.")));
    CHECK(prompt.contains(QString::fromUtf8("中文")));
}

static void testAppPaths()
{
    const QStringList whisper = AppPaths::whisperModelCandidates();
    const QStringList translator = AppPaths::translatorModelCandidates();
    CHECK(!whisper.isEmpty());
    CHECK(!translator.isEmpty());
    CHECK(whisper.first().startsWith("models/"));      // 便携包优先
    CHECK(translator.join(";").contains("gguf"));
    CHECK(whisper.join(";").contains("ggml-small.bin"));

    // 找不到时必须返回空串并记下尝试过的位置
    const QString missing = AppPaths::resolveResource(QString(), QStringList() << "definitely-missing-model.gguf");
    CHECK_EQ(missing, QString());
    CHECK(!AppPaths::lastTriedLocations().isEmpty());

    // 能解析出当前工作目录里的真实文件
    const QString probeName = "unit_test_probe.gguf";
    {
        QFile probe(probeName);
        if (probe.open(QIODevice::WriteOnly)) {
            probe.write("probe");
            probe.close();
        }
    }
    const QString found = AppPaths::resolveResource(QString(), QStringList() << probeName);
    CHECK(found.endsWith(probeName));
    CHECK(QFile::exists(found));
    QFile::remove(probeName);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    testLooksLikeLanguage();
    testPostProcess();
    testBuildPrompt();
    testAppPaths();

    std::printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
