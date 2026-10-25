#include "AppPaths.h"
#include "Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

namespace {

QStringList g_lastTried;

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

QStringList lastTriedLocations()
{
    return g_lastTried;
}

QString resolveResource(const QString &settingsKey, const QStringList &relativeCandidates)
{
    g_lastTried.clear();

    // 1) 用户显式配置优先；配错了要明确告诉他，而不是静默回退到别的路径
    QSettings settings("MediaPlayer", "Config");
    const QString configured = settings.value(settingsKey).toString().trimmed();
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
    return {
        QStringLiteral("models/ggml-small.bin"),
        QStringLiteral("../../third_party/whisper.cpp/models/ggml-small.bin"),
        QStringLiteral("../third_party/whisper.cpp/models/ggml-small.bin"),
        QStringLiteral("third_party/whisper.cpp/models/ggml-small.bin"),
        QStringLiteral("models/ggml-base.bin"),
        QStringLiteral("../../third_party/whisper.cpp/models/ggml-base.bin"),
    };
}

QStringList translatorModelCandidates()
{
    return {
        QStringLiteral("models/Hy-MT2-1.8B-Q4_K_M.gguf"),
        QStringLiteral("../../third_party/models/Hy-MT2-1.8B-Q4_K_M.gguf"),
        QStringLiteral("../third_party/models/Hy-MT2-1.8B-Q4_K_M.gguf"),
        QStringLiteral("third_party/models/Hy-MT2-1.8B-Q4_K_M.gguf"),
    };
}

} // namespace AppPaths
