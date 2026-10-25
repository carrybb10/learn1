#include "DiagnosticExporter.h"
#include "PlayerController.h"
#include "Logger.h"
#include "SystemInfo.h"
#include "CrashHandler.h"
#include "Version.h"
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QCoreApplication>
#include <QProcess>
#include <windows.h>
#include <shlobj.h>
#include <cstdio>

#pragma comment(lib, "shell32.lib")

static QString getDesktopPath() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path))) {
        QString result = QString::fromWCharArray(path);
        CoTaskMemFree(path);
        return result;
    }
    return QDir::homePath() + "/Desktop";
}

QString DiagnosticExporter::exportDiagnostic(PlayerController* player, const QString& outputDir) {
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString baseName = QString("diagnostic_%1").arg(timestamp);

    QString destRoot = outputDir.isEmpty() ? getDesktopPath() : outputDir;
    fprintf(stderr, "[Export] destRoot: %s\n", destRoot.toUtf8().constData());

    QString destDir = QDir(destRoot).filePath(baseName);
    QDir().mkpath(destDir);
    fprintf(stderr, "[Export] destDir: %s\n", destDir.toUtf8().constData());

    // 1. 系统信息
    QFile sysFile(QDir(destDir).filePath("system_info.txt"));
    if (sysFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&sysFile);
        out << generateSystemInfo();
        sysFile.close();
    }

    // 2. 配置
    QFile cfgFile(QDir(destDir).filePath("config.txt"));
    if (cfgFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&cfgFile);
        out << generateConfig(player);
        cfgFile.close();
    }

    // 3. 播放状态
    QFile stateFile(QDir(destDir).filePath("player_state.txt"));
    if (stateFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&stateFile);
        out << generatePlayerState(player);
        stateFile.close();
    }

    // 4. 日志
    QString logsDir = QDir(destDir).filePath("logs");
    QDir().mkpath(logsDir);
    copyLogs(logsDir);

    // 5. 崩溃报告
    QString crashDir = QDir(destDir).filePath("crash_reports");
    QDir().mkpath(crashDir);
    copyCrashReports(crashDir);

    // 6. README
    QFile readme(QDir(destDir).filePath("README.txt"));
    if (readme.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&readme);
        out << "MediaPlayer Diagnostic Report\n";
        out << "=============================\n";
        out << "Export time: " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n";
        out << "Version: v" MP_APP_VERSION "\n\n";
        out << "Contents:\n";
        out << "  system_info.txt    - CPU/GPU/内存/OS信息\n";
        out << "  config.txt         - 当前播放器配置\n";
        out << "  player_state.txt   - 播放状态快照\n";
        out << "  logs/              - 运行日志\n";
        out << "  crash_reports/     - 崩溃报告（如果有）\n\n";
        out << "请将此文件夹打包后发送给开发者。\n";
        readme.close();
    }

    // 7. 尝试打包为 zip（使用 PowerShell Compress-Archive）
    QString zipPath = QDir(destRoot).filePath(baseName + ".zip");
    fprintf(stderr, "[Export] Creating zip: %s\n", zipPath.toUtf8().constData());
    if (createZip(destDir, zipPath)) {
        fprintf(stderr, "[Export] Zip created OK, removing temp dir\n");
        QDir(destDir).removeRecursively();
        return zipPath;
    }
    fprintf(stderr, "[Export] Zip creation FAILED, returning folder: %s\n", destDir.toUtf8().constData());
    return destDir;
}

QString DiagnosticExporter::generateSystemInfo() {
    SystemInfoData info = SystemInfo::collect();
    return SystemInfo::format(info);
}

QString DiagnosticExporter::generateConfig(PlayerController* player) {
    QString out;
    out += "=== Player Configuration ===\n";
    if (player) {
        auto stats = player->diagnosticStats();
        out += QString("Hardware decode: %1\n").arg(stats.hwDecodeEnabled ? "D3D11VA" : "Software");
        out += QString("Video resolution: %1x%2\n").arg(stats.videoWidth).arg(stats.videoHeight);
    }
    out += "\n=== Qt Configuration ===\n";
    out += QString("Qt version: %1\n").arg(qVersion());
    out += QString("Application dir: %1\n").arg(QCoreApplication::applicationDirPath());
    return out;
}

QString DiagnosticExporter::generatePlayerState(PlayerController* player) {
    QString out;
    out += "=== Player State ===\n";
    if (!player) {
        out += "Player not available\n";
        return out;
    }

    auto stats = player->diagnosticStats();
    out += QString("Current file: %1\n").arg(stats.currentFile);
    out += QString("Position: %1 ms / %2 ms\n").arg(stats.positionMs).arg(stats.durationMs);
    out += QString("Decode FPS: %1\n").arg(stats.decodeFps);
    out += QString("Display FPS: %1\n").arg(stats.displayFps);
    out += QString("Video frame queue: %1\n").arg(stats.videoFrameQueueSize);
    out += QString("Audio frame queue: %1\n").arg(stats.audioFrameQueueSize);
    out += QString("Video packet queue: %1\n").arg(stats.videoPacketQueueSize);
    out += QString("Audio packet queue: %1\n").arg(stats.audioPacketQueueSize);
    out += QString("Audio clock: %1 s\n").arg(stats.audioClock);
    out += QString("Video clock: %1 s\n").arg(stats.videoClock);
    out += QString("Is playing: %1\n").arg(player->isPlaying() ? "yes" : "no");

    out += "\n=== Recent Logs ===\n";
    QStringList logs = Logger::instance().recentLogs(200);
    for (const QString& line : logs) {
        out += line + "\n";
    }
    return out;
}

bool DiagnosticExporter::copyLogs(const QString& destDir) {
    QString logDir = QCoreApplication::applicationDirPath() + "/../../logs";
    QDir dir(logDir);
    if (!dir.exists()) return false;

    QStringList filters;
    filters << "*.log";
    QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Time);

    bool ok = true;
    for (int i = 0; i < qMin(3, files.size()); i++) {
        QString dest = QDir(destDir).filePath(files[i].fileName());
        if (!QFile::copy(files[i].absoluteFilePath(), dest)) {
            ok = false;
        }
    }
    return ok;
}

bool DiagnosticExporter::copyCrashReports(const QString& destDir) {
    QStringList reports = CrashHandler::recentCrashReports(5);
    bool ok = true;
    for (const QString& report : reports) {
        QFileInfo fi(report);
        QString dest = QDir(destDir).filePath(fi.fileName());
        if (!QFile::copy(report, dest)) {
            ok = false;
        }
    }
    return ok;
}

bool DiagnosticExporter::createZip(const QString& srcDir, const QString& zipPath) {
    // 使用 PowerShell Compress-Archive
    QProcess process;
    QString script = QString("Compress-Archive -Path '%1\\*' -DestinationPath '%2' -Force")
        .arg(srcDir, zipPath);
    fprintf(stderr, "[Export] Running: powershell -Command \"%s\"\n", script.toUtf8().constData());
    process.start("powershell", QStringList() << "-Command" << script);
    process.waitForFinished(30000);
    int exitCode = process.exitCode();
    QByteArray stdoutData = process.readAllStandardOutput();
    QByteArray stderrData = process.readAllStandardError();
    fprintf(stderr, "[Export] PowerShell exitCode=%d\n", exitCode);
    if (!stdoutData.isEmpty()) fprintf(stderr, "[Export] stdout: %s\n", stdoutData.constData());
    if (!stderrData.isEmpty()) fprintf(stderr, "[Export] stderr: %s\n", stderrData.constData());
    bool exists = QFile::exists(zipPath);
    fprintf(stderr, "[Export] zip exists: %s\n", exists ? "yes" : "no");
    return exitCode == 0 && exists;
}
