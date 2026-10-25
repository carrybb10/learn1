#include "CrashHandler.h"
#include "Logger.h"
#include <QDir>
#include <QCoreApplication>
#include <QDateTime>
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

void CrashHandler::install() {
    SetUnhandledExceptionFilter(CrashHandler::exceptionFilter);
    LOG_INFO(LogModule::CRASH, "Crash handler installed");
}

QString CrashHandler::crashReportDir() {
    QString dir = QCoreApplication::applicationDirPath() + "/../../crash_reports";
    QDir().mkpath(dir);
    return dir;
}

QStringList CrashHandler::recentCrashReports(int max) {
    QDir dir(crashReportDir());
    QStringList filters;
    filters << "crash_*.txt";
    QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Time);
    QStringList result;
    for (int i = 0; i < qMin(max, files.size()); i++) {
        result.append(files[i].absoluteFilePath());
    }
    return result;
}

QString CrashHandler::exceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION (0xC0000005)";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
        default:                                 return QString("UNKNOWN (0x%1)").arg(code, 0, 16);
    }
}

long __stdcall CrashHandler::exceptionFilter(EXCEPTION_POINTERS* exceptionInfo) {
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString reportPath = QDir(crashReportDir()).filePath(QString("crash_%1.txt").arg(timestamp));
    QString dumpPath = QDir(crashReportDir()).filePath(QString("crash_%1.dmp").arg(timestamp));

    // 写 minidump
    HANDLE hFile = CreateFileW(dumpPath.toStdWString().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = exceptionInfo;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
            MiniDumpNormal, &mei, NULL, NULL);
        CloseHandle(hFile);
    }

    // 写文本报告
    writeCrashReport(exceptionInfo, reportPath);

    // 也输出到 stderr
    fprintf(stderr, "\n=== CRASH! Report: %s ===\n", reportPath.toUtf8().constData());

    return EXCEPTION_EXECUTE_HANDLER;
}

// 独立函数：栈回溯（无C++对象，可使用__try），直接写入FILE*
static void dumpStackTrace(FILE* fp, ULONG64* rsp) {
    for (int i = 0; i < 32; i++) {
        ULONG64 val = 0;
        __try {
            val = rsp[i];
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            fprintf(fp, "  (unable to read stack beyond this point)\n");
            break;
        }
        if (val > 0x10000) {
            fprintf(fp, "  [%d] 0x%llx\n", i, val);
        }
    }
}

void CrashHandler::writeCrashReport(EXCEPTION_POINTERS* exceptionInfo, const QString& reportPath) {
    QFile file(reportPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;

    QTextStream out(&file);
    out << "=== MediaPlayer Crash Report ===\n";
    out << "Time: " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n";
    out << "Version: v0.4.9 (T4.9 diagnostics)\n";
    out << "Exception: " << exceptionName(exceptionInfo->ExceptionRecord->ExceptionCode) << "\n";
    out << "Exception Address: 0x" << QString::number((quintptr)exceptionInfo->ExceptionRecord->ExceptionAddress, 16) << "\n";

    if (exceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR accessType = exceptionInfo->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR accessAddr = exceptionInfo->ExceptionRecord->ExceptionInformation[1];
        out << "Access Type: " << (accessType == 0 ? "READ" : (accessType == 1 ? "WRITE" : "EXECUTE")) << "\n";
        out << "Access Address: 0x" << QString::number(accessAddr, 16) << "\n";
    }

    out << "\n--- Registers ---\n";
    CONTEXT* ctx = exceptionInfo->ContextRecord;
    out << QString("RAX: 0x%1  RBX: 0x%2  RCX: 0x%3  RDX: 0x%4\n")
        .arg(ctx->Rax, 0, 16).arg(ctx->Rbx, 0, 16).arg(ctx->Rcx, 0, 16).arg(ctx->Rdx, 0, 16);
    out << QString("RSI: 0x%1  RDI: 0x%2  RBP: 0x%3  RSP: 0x%4\n")
        .arg(ctx->Rsi, 0, 16).arg(ctx->Rdi, 0, 16).arg(ctx->Rbp, 0, 16).arg(ctx->Rsp, 0, 16);
    out << QString("RIP: 0x%1\n").arg(ctx->Rip, 0, 16);

    // 调用栈（用FILE*写入，避免C++对象与__try冲突）
    out << "\n--- Stack Trace (raw addresses) ---\n";
    out.flush();
    FILE* fp = _wfopen(reportPath.toStdWString().c_str(), L"a");
    if (fp) {
        dumpStackTrace(fp, (ULONG64*)ctx->Rsp);
        fclose(fp);
    }

    // 重新打开追加最近日志
    QFile logFile(reportPath);
    if (logFile.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream logOut(&logFile);
        logOut << "\n--- Recent Logs ---\n";
        QStringList logs = Logger::instance().recentLogs(100);
        for (const QString& line : logs) {
            logOut << line << "\n";
        }
        logOut << "\n=== End of Report ===\n";
        logFile.close();
    }
    file.close();
}
