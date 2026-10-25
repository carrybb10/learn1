#ifndef SUBTITLEPARSER_H
#define SUBTITLEPARSER_H

#include <QString>
#include <QList>
#include <QRegularExpression>

struct SubtitleCue {
    double startTime;  // 秒
    double endTime;    // 秒
    QString text;
    QString translatedText;  // 翻译文本（AI翻译功能）
};

class SubtitleParser
{
public:
    SubtitleParser();

    // 解析字幕文件（支持SRT/ASS/SSA）
    bool loadFile(const QString &filePath);

    // 直接设置字幕条目（用于AI识别结果）
    void setCues(const QList<SubtitleCue> &cues);

    // 设置翻译文本（按索引对应 setCues 的条目）
    void setTranslations(const QList<QString> &translations);

    // 获取当前时间应显示的字幕（原文）
    QString getCueAtTime(double timeSec) const;

    // 获取当前时间应显示的字幕（译文）
    QString getTranslationAtTime(double timeSec) const;

    // 获取当前时间的完整字幕条目
    SubtitleCue getCueObjectAtTime(double timeSec) const;

    // 清空
    void clear();

    bool isLoaded() const { return !m_cues.isEmpty(); }
    int cueCount() const { return m_cues.size(); }
    QList<SubtitleCue> cues() const { return m_cues; }

private:
    QList<SubtitleCue> m_cues;

    bool parseSRT(const QString &content);
    bool parseASS(const QString &content);
    double parseTimestamp(const QString &ts) const;  // SRT格式 HH:MM:SS,mmm
    double parseASSTimestamp(const QString &ts) const;  // ASS格式 H:MM:SS.cc
};

#endif // SUBTITLEPARSER_H
