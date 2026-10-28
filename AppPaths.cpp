#include "AppPaths.h"
#include "Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

namespace {

QStringList g_lastTried;
QString g_configFile;

QString absolute(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString tryRelative(const QString &baseDir, const QString &rel)
{
    const QString candidate = absolute(QDir(baseDir).filePath(rel));
    g_lastTried << candidate;
    if (QFile::exists(candidate))
        return candidate;
    return QString();
}

} // namespace

namespace AppPaths {

bool preferHeavyModels()
{
#ifdef MP_HAVE_CUDA
    return true;
#else
    return false;
#endif
}

void setConfigFile(const QString &iniPath)
{
    g_configFile = iniPath;
}

QString configFile()
{
    return g_configFile;
}

QString modelsDirHint()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("models"));
}

QStringList lastTriedLocations()
{
    return g_lastTried;
}

QString resolveResource(const QString &settingsKey, const QStringList &relativeCandidates)
{
    g_lastTried.clear();

    // 0) 便携 ini：便携包解压到任意目录后，用户改一个文本文件就能指过去，不用碰注册表
    QString configured;
    if (!settingsKey.isEmpty() && !g_configFile.isEmpty() && QFile::exists(g_configFile)) {
        QSettings ini(g_configFile, QSettings::IniFormat);
        configured = ini.value(settingsKey).toString().trimmed();
        if (!configured.isEmpty()) {
            QString path = configured;
            if (QFileInfo(path).isRelative())
                path = QDir(QCoreApplication::applicationDirPath()).filePath(path);
            path = absolute(path);
            if (QFile::exists(path)) {
                LOG_INFO(LogModule::CORE, "resource %s from %s: %s",
                         settingsKey.toUtf8().constData(), g_configFile.toUtf8().constData(),
                         path.toUtf8().constData());
                return path;
            }
            g_lastTried << path;
            LOG_WARN(LogModule::CORE, "ini %s points to a missing file: %s",
                     settingsKey.toUtf8().constData(), path.toUtf8().constData());
        }
    }

    // 1) 用户显式配置优先；配错了要明确告诉他，而不是静默回退到别的路径
    // 空 key 跳过：QSettings 遇到空 key 会打 "Empty key passed" 告警（单测走这条路径）
    if (!settingsKey.isEmpty()) {
        QSettings settings("MediaPlayer", "Config");
        configured = settings.value(settingsKey).toString().trimmed();
    } else {
        configured.clear();
    }
    if (!configured.isEmpty()) {
        QString path = configured;
        if (QFileInfo(path).isRelative())
            path = QDir(QCoreApplication::applicationDirPath()).filePath(path);
        path = absolute(path);
        if (QFile::exists(path)) {
            LOG_INFO(LogModule::CORE, "resource %s from config: %s",
                     settingsKey.toUtf8().constData(), path.toUtf8().constData());
            return path;
        }
        g_lastTried << path;
        LOG_WARN(LogModule::CORE, "config %s points to a missing file: %s (falling back to defaults)",
                 settingsKey.toUtf8().constData(), path.toUtf8().constData());
    }

    // 2) 应用目录附近的候选：覆盖便携包与仓库构建目录两种形态
    const QString appDir = QCoreApplication::applicationDirPath();
    for (const QString &rel : relativeCandidates) {
        const QString found = tryRelative(appDir, rel);
        if (!found.isEmpty()) {
            LOG_INFO(LogModule::CORE, "resource %s found at: %s",
                     settingsKey.toUtf8().constData(), found.toUtf8().constData());
            return found;
        }
    }

    // 3) 当前工作目录：某些启动方式（快捷方式/调试器）会把 cwd 设成仓库根
    const QString cwd = QDir::currentPath();
    if (QDir::cleanPath(cwd) != QDir::cleanPath(appDir)) {
        for (const QString &rel : relativeCandidates) {
            const QString found = tryRelative(cwd, rel);
            if (!found.isEmpty()) {
                LOG_INFO(LogModule::CORE, "resource %s found in cwd: %s",
                         settingsKey.toUtf8().constData(), found.toUtf8().constData());
                return found;
            }
        }
    }

    LOG_WARN(LogModule::CORE, "resource %s not found after %d candidates; set config key %s to override",
             settingsKey.toUtf8().constData(), g_lastTried.size(),
             settingsKey.toUtf8().constData());
    return QString();
}

QStringList whisperModelCandidates()
{
    // 便携包（模型与 exe 同级）→ 仓库构建目录 build/Debug → 仓库根 → models/
    // GPU 构建优先 large-v3-turbo：它把日语识别从"听错词"救回来（small 会把 あくび 听成 役日）；
    // CPU 构建优先 small：turbo 在 CPU 上只有约 0.9 倍实时，延迟会明显变差。
    const QStringList turbo = {
        QStringLiteral("models/ggml-large-v3-turbo.bin"),
        QStringLiteral("../../third_party/whisper.cpp/models/ggml-large-v3-turbo.bin"),
        QStringLiteral("../third_party/whisper.cpp/models/ggml-large-v3-turbo.bin"),
        QStringLiteral("third_party/whisper.cpp/models/ggml-large-v3-turbo.bin"),
    };
    const QStringList small = {
        QStringLiteral("models/ggml-small.bin"),
        QStringLiteral("../../third_party/whisper.cpp/models/ggml-small.bin"),
        QStringLiteral("../third_party/whisper.cpp/models/ggml-small.bin"),
        QStringLiteral("third_party/whisper.cpp/models/ggml-small.bin"),
    };
    const QStringList base = {
        QStringLiteral("models/ggml-base.bin"),
        QStringLiteral("../../third_party/whisper.cpp/models/ggml-base.bin"),
    };
    return preferHeavyModels() ? (turbo + small + base) : (small + turbo + base);
}

QStringList translatorModelCandidates()
{
    // GPU 构建优先 Q8：显存够时译文比 Q4 稳；CPU 构建优先 Q4：Q8 权重 1.9GB，CPU 上更慢。
    const QStringList q8 = {
        QStringLiteral("models/Hy-MT2-1.8B-Q8_0.gguf"),
        QStringLiteral("../../third_party/models/Hy-MT2-1.8B-Q8_0.gguf"),
        QStringLiteral("../third_party/models/Hy-MT2-1.8B-Q8_0.gguf"),
        QStringLiteral("third_party/models/Hy-MT2-1.8B-Q8_0.gguf"),
    };
    const QStringList q4 = {
        QStringLiteral("models/Hy-MT2-1.8B-Q4_K_M.gguf"),
        QStringLiteral("../../third_party/models/Hy-MT2-1.8B-Q4_K_M.gguf"),
        QStringLiteral("../third_party/models/Hy-MT2-1.8B-Q4_K_M.gguf"),
        QStringLiteral("third_party/models/Hy-MT2-1.8B-Q4_K_M.gguf"),
    };
    return preferHeavyModels() ? (q8 + q4) : (q4 + q8);
}

QStringList vadModelCandidates()
{
    return {
        QStringLiteral("models/ggml-silero-v5.1.2.bin"),
        QStringLiteral("../../third_party/whisper.cpp/models/ggml-silero-v5.1.2.bin"),
        QStringLiteral("../third_party/whisper.cpp/models/ggml-silero-v5.1.2.bin"),
        QStringLiteral("third_party/whisper.cpp/models/ggml-silero-v5.1.2.bin"),
    };
}

} // namespace AppPaths
