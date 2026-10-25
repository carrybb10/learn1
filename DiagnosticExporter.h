#ifndef DIAGNOSTICEXPORTER_H
#define DIAGNOSTICEXPORTER_H

#include <QString>

class PlayerController;

class DiagnosticExporter {
public:
    static QString exportDiagnostic(PlayerController* player, const QString& outputDir = QString());

private:
    static QString generateSystemInfo();
    static QString generateConfig(PlayerController* player);
    static QString generatePlayerState(PlayerController* player);
    static bool copyLogs(const QString& destDir);
    static bool copyCrashReports(const QString& destDir);
    static bool createZip(const QString& srcDir, const QString& zipPath);
};

#endif // DIAGNOSTICEXPORTER_H
