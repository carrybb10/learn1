#ifndef CRASHHANDLER_H
#define CRASHHANDLER_H

#include <QString>
#include <windows.h>

class CrashHandler {
public:
    static void install();
    static QString crashReportDir();
    static QStringList recentCrashReports(int max = 5);

private:
    static long __stdcall exceptionFilter(EXCEPTION_POINTERS* exceptionInfo);
    static void writeCrashReport(EXCEPTION_POINTERS* exceptionInfo, const QString& reportPath);
    static QString exceptionName(DWORD code);
};

#endif // CRASHHANDLER_H
