#include "Logger.h"
#include <QDir>
#include <QCoreApplication>
#include <cstdio>
#include <windows.h>

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::Logger() : m_level(LogLevel::LVL_INFO) {
    ensureLogDir();
    rotateLogs();

    QString logDir = QCoreApplication::applicationDirPath() + "/logs";
    m_logFilePath = QDir(logDir).filePath(
        QString("mediaplayer_%1.log").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));

    m_logFile.setFileName(m_logFilePath);
    if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QString header = QString("=== MediaPlayer Log Start: %1 ===\n")
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        m_logFile.write(header.toUtf8());
        m_logFile.flush();
    }

    // 从环境变量读取日志级别
    QByteArray envLevel = qgetenv("MEDIAPLAYER_LOG_LEVEL");
    if (!envLevel.isEmpty()) {
        QString lvl = QString(envLevel).toLower();
        if (lvl == "trace") m_level = LogLevel::LVL_TRACE;
        else if (lvl == "debug") m_level = LogLevel::LVL_DEBUG;
        else if (lvl == "info") m_level = LogLevel::LVL_INFO;
        else if (lvl == "warn") m_level = LogLevel::LVL_WARN;
        else if (lvl == "error") m_level = LogLevel::LVL_ERROR;
    }
}

Logger::~Logger() {
    QMutexLocker locker(&m_mutex);
    if (m_logFile.isOpen()) {
        m_logFile.write("=== Log End ===\n");
        m_logFile.close();
    }
}

void Logger::setLogLevel(LogLevel level) {
    QMutexLocker locker(&m_mutex);
    m_level = level;
}

void Logger::log(LogLevel level, LogModule module, const char* file, int line, const QString& msg) {
    QMutexLocker locker(&m_mutex);

    // 截断文件路径，只保留文件名
    QString fileName = QString(file).section('/', -1).section('\\', -1);

    // 获取线程ID
    DWORD threadId = GetCurrentThreadId();

    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    QString lineStr = QString("[%1] [%2] [%3] [T%4] [%5:%6] %7\n")
        .arg(timestamp)
        .arg(levelName(level))
        .arg(moduleName(module))
        .arg(threadId, 4, 10, QChar('0'))
        .arg(fileName)
        .arg(line)
        .arg(msg);

    // 输出到 stderr
    fprintf(stderr, "%s", lineStr.toUtf8().constData());
    fflush(stderr);

    // 写入文件
    if (m_logFile.isOpen()) {
        m_logFile.write(lineStr.toUtf8());
        m_logFile.flush();
    }

    // 保存到环形缓冲
    m_recentLogs.append(lineStr.trimmed());
    if (m_recentLogs.size() > MAX_RECENT) {
        m_recentLogs.removeFirst();
    }
}

QStringList Logger::recentLogs(int maxLines) const {
    QMutexLocker locker(&m_mutex);
    int start = qMax(0, m_recentLogs.size() - maxLines);
    return m_recentLogs.mid(start);
}

QString Logger::levelName(LogLevel level) {
    switch (level) {
        case LogLevel::LVL_TRACE: return "TRACE";
        case LogLevel::LVL_DEBUG: return "DEBUG";
        case LogLevel::LVL_INFO:  return "INFO";
        case LogLevel::LVL_WARN:  return "WARN";
        case LogLevel::LVL_ERROR: return "ERROR";
        case LogLevel::LVL_FATAL: return "FATAL";
    }
    return "UNKNOWN";
}

QString Logger::moduleName(LogModule module) {
    switch (module) {
        case LogModule::CORE:     return "CORE";
        case LogModule::DECODER:  return "DECODER";
        case LogModule::RENDERER: return "RENDERER";
        case LogModule::AUDIO:    return "AUDIO";
        case LogModule::SUBTITLE: return "SUBTITLE";
        case LogModule::AI_ASR:   return "AI_ASR";
        case LogModule::AI_MT:    return "AI_MT";
        case LogModule::UI:       return "UI";
        case LogModule::CRASH:    return "CRASH";
    }
    return "UNKNOWN";
}

void Logger::ensureLogDir() {
    QString logDir = QCoreApplication::applicationDirPath() + "/logs";
    QDir().mkpath(logDir);
}

void Logger::rotateLogs() {
    QString logDir = QCoreApplication::applicationDirPath() + "/logs";
    QDir dir(logDir);
    if (!dir.exists()) return;

    // 获取所有日志文件，按修改时间排序
    QStringList filters;
    filters << "mediaplayer_*.log";
    QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Time);

    // 只保留最近5个
    for (int i = 5; i < files.size(); i++) {
        QFile::remove(files[i].absoluteFilePath());
    }
}
