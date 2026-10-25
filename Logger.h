#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QFile>
#include <QMutex>
#include <QDateTime>

// 日志级别
enum class LogLevel {
    LVL_TRACE = 0,
    LVL_DEBUG = 1,
    LVL_INFO = 2,
    LVL_WARN = 3,
    LVL_ERROR = 4,
    LVL_FATAL = 5
};

// 日志模块
enum class LogModule {
    CORE = 0,
    DECODER = 1,
    RENDERER = 2,
    AUDIO = 3,
    SUBTITLE = 4,
    AI_ASR = 5,
    AI_MT = 6,
    UI = 7,
    CRASH = 8
};

class Logger {
public:
    static Logger& instance();

    void setLogLevel(LogLevel level);
    LogLevel logLevel() const { return m_level; }

    void log(LogLevel level, LogModule module, const char* file, int line, const QString& msg);

    // 获取最近N行日志（用于诊断面板和崩溃报告）
    QStringList recentLogs(int maxLines = 200) const;

    // 日志文件路径
    QString logFilePath() const { return m_logFilePath; }

    // 模块名/级别名
    static QString levelName(LogLevel level);
    static QString moduleName(LogModule module);

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void rotateLogs();
    void ensureLogDir();

    mutable QMutex m_mutex;
    LogLevel m_level;
    QFile m_logFile;
    QString m_logFilePath;
    QStringList m_recentLogs;  // 环形缓冲，保留最近500行
    static const int MAX_RECENT = 500;
};

// 便捷宏
#define LOG_TRACE(module, ...) \
    do { \
        if (Logger::instance().logLevel() <= LogLevel::LVL_TRACE) { \
            QString _msg = QString::asprintf(__VA_ARGS__); \
            Logger::instance().log(LogLevel::LVL_TRACE, module, __FILE__, __LINE__, _msg); \
        } \
    } while(0)

#define LOG_DEBUG(module, ...) \
    do { \
        if (Logger::instance().logLevel() <= LogLevel::LVL_DEBUG) { \
            QString _msg = QString::asprintf(__VA_ARGS__); \
            Logger::instance().log(LogLevel::LVL_DEBUG, module, __FILE__, __LINE__, _msg); \
        } \
    } while(0)

#define LOG_INFO(module, ...) \
    do { \
        if (Logger::instance().logLevel() <= LogLevel::LVL_INFO) { \
            QString _msg = QString::asprintf(__VA_ARGS__); \
            Logger::instance().log(LogLevel::LVL_INFO, module, __FILE__, __LINE__, _msg); \
        } \
    } while(0)

#define LOG_WARN(module, ...) \
    do { \
        if (Logger::instance().logLevel() <= LogLevel::LVL_WARN) { \
            QString _msg = QString::asprintf(__VA_ARGS__); \
            Logger::instance().log(LogLevel::LVL_WARN, module, __FILE__, __LINE__, _msg); \
        } \
    } while(0)

#define LOG_ERROR(module, ...) \
    do { \
        if (Logger::instance().logLevel() <= LogLevel::LVL_ERROR) { \
            QString _msg = QString::asprintf(__VA_ARGS__); \
            Logger::instance().log(LogLevel::LVL_ERROR, module, __FILE__, __LINE__, _msg); \
        } \
    } while(0)

#define LOG_FATAL(module, ...) \
    do { \
        QString _msg = QString::asprintf(__VA_ARGS__); \
        Logger::instance().log(LogLevel::LVL_FATAL, module, __FILE__, __LINE__, _msg); \
    } while(0)

#endif // LOGGER_H
