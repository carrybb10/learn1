#include "CrashHandler.h"
#include "Logger.h"
#include "Version.h"
#include <QDir>
#include <QCoreApplication>
#include <QDateTime>
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <wchar.h>

#pragma comment(lib, "dbghelp.lib")

// 崩溃处理中的重入保护：符号解析本身也可能崩溃（堆已损坏时），
// 第二次进入直接终止，避免无限递归把报告写坏。
static volatile LONG g_crashHandling = 0;

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
    // 只处理第一次崩溃；重入说明处理器自身崩溃，直接交给系统
    if (InterlockedExchange(&g_crashHandling, 1) != 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

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
            (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
            &mei, NULL, NULL);
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

// ---------------------------------------------------------------------------
// 符号化栈回溯
// 约束：崩溃时堆可能已损坏，本段只用 POD 局部变量与固定大小缓冲，
// 不构造 QString/QTextStream；任何一步失败都退回上面的裸地址输出。
// ---------------------------------------------------------------------------
static bool initSymbols() {
    static int state = 0;                    // 0=未尝试 / 1=可用 / -1=不可用
    if (state != 0) return state > 0;

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_NO_PROMPTS);

    wchar_t exePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) { state = -1; return false; }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash) *slash = L'\0';               // exe 目录：Debug 构建的 .pdb 就在旁边

    wchar_t searchPath[MAX_PATH * 4] = {0};
    wcscpy_s(searchPath, MAX_PATH * 4, exePath);
    wchar_t envPath[MAX_PATH * 2] = {0};
    if (GetEnvironmentVariableW(L"_NT_SYMBOL_PATH", envPath, MAX_PATH * 2) > 0) {
        wcscat_s(searchPath, MAX_PATH * 4, L";");
        wcscat_s(searchPath, MAX_PATH * 4, envPath);
    }

    // InvadeProcess=TRUE：一次性登记当前进程已加载的全部模块
    state = SymInitializeW(GetCurrentProcess(), searchPath, TRUE) ? 1 : -1;
    return state > 0;
}

static void formatFrame(char* out, size_t outLen, int index, DWORD64 pc) {
    IMAGEHLP_MODULE64 mod = {0};
    mod.SizeOfStruct = sizeof(mod);
    const char* modName = "?";
    DWORD64 modBase = 0;
    if (SymGetModuleInfo64(GetCurrentProcess(), pc, &mod)) {
        modName = mod.ModuleName;
        modBase = mod.BaseOfImage;
    }

    char symBuf[sizeof(SYMBOL_INFO) + 256] = {0};
    SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;

    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), pc, &disp, sym)) {
        IMAGEHLP_LINE64 lineInfo = {0};
        lineInfo.SizeOfStruct = sizeof(lineInfo);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), pc, &lineDisp, &lineInfo)) {
            _snprintf_s(out, outLen, _TRUNCATE, "#%02d 0x%016llx  %s!%s+0x%llx  (%s:%lu)\n",
                        index, (unsigned long long)pc, modName, sym->Name,
                        (unsigned long long)disp,
                        lineInfo.FileName ? lineInfo.FileName : "?",
                        (unsigned long)lineInfo.LineNumber);
            return;
        }
        _snprintf_s(out, outLen, _TRUNCATE, "#%02d 0x%016llx  %s!%s+0x%llx\n",
                    index, (unsigned long long)pc, modName, sym->Name, (unsigned long long)disp);
        return;
    }
    _snprintf_s(out, outLen, _TRUNCATE, "#%02d 0x%016llx  %s+0x%llx\n",
                index, (unsigned long long)pc, modName, (unsigned long long)(pc - modBase));
}

static void dumpSymbolizedStack(FILE* fp, const CONTEXT* ctx) {
    if (!initSymbols()) {
        fprintf(fp, "  (dbghelp init failed, fallback to raw addresses)\n");
        dumpStackTrace(fp, (ULONG64*)ctx->Rsp);
        return;
    }

    CONTEXT walkCtx = *ctx;
    STACKFRAME64 frame = {0};
    frame.AddrPC.Offset    = walkCtx.Rip; frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = walkCtx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = walkCtx.Rsp; frame.AddrStack.Mode = AddrModeFlat;

    for (int i = 0; i < 40; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(),
                         &frame, &walkCtx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) break;

        char line[1024] = {0};
        formatFrame(line, sizeof(line), i, frame.AddrPC.Offset);
        fputs(line, fp);
    }
    fflush(fp);
}

void CrashHandler::writeCrashReport(EXCEPTION_POINTERS* exceptionInfo, const QString& reportPath) {
    QFile file(reportPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;

    QTextStream out(&file);
    out << "=== MediaPlayer Crash Report ===\n";
    out << "Time: " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n";
    out << "Version: v" MP_APP_VERSION "\n";
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

    // 调用栈：优先符号化（模块!函数+偏移 源文件:行号），失败自动退回裸地址
    out << "\n--- Stack Trace ---\n";
    out.flush();
    FILE* fp = _wfopen(reportPath.toStdWString().c_str(), L"a");
    if (fp) {
        dumpSymbolizedStack(fp, ctx);
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
