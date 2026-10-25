#include "SubtitleParser.h"
#include <QFile>
#include <QTextStream>
#include <QStringList>

SubtitleParser::SubtitleParser()
{
}

void SubtitleParser::setCues(const QList<SubtitleCue> &cues)
{
    m_cues = cues;
}

bool SubtitleParser::loadFile(const QString &filePath)
{
    clear();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    QString content = in.readAll();
    file.close();

    if (content.isEmpty()) return false;

    QString lower = filePath.toLower();
    if (lower.endsWith(".ass") || lower.endsWith(".ssa")) {
        return parseASS(content);
    } else {
        return parseSRT(content);
    }
}

QString SubtitleParser::getCueAtTime(double timeSec) const
{
    for (const SubtitleCue &cue : m_cues) {
        if (timeSec >= cue.startTime && timeSec <= cue.endTime) {
            return cue.text;
        }
    }
    return QString();
}

QString SubtitleParser::getTranslationAtTime(double timeSec) const
{
    for (const SubtitleCue &cue : m_cues) {
        if (timeSec >= cue.startTime && timeSec <= cue.endTime) {
            return cue.translatedText;
        }
    }
    return QString();
}

SubtitleCue SubtitleParser::getCueObjectAtTime(double timeSec) const
{
    for (const SubtitleCue &cue : m_cues) {
        if (timeSec >= cue.startTime && timeSec <= cue.endTime) {
            return cue;
        }
    }
    return SubtitleCue();
}

void SubtitleParser::setTranslations(const QList<QString> &translations)
{
    for (int i = 0; i < m_cues.size() && i < translations.size(); i++) {
        m_cues[i].translatedText = translations[i];
    }
}

void SubtitleParser::clear()
{
    m_cues.clear();
}

bool SubtitleParser::parseSRT(const QString &content)
{
    // SRT格式：序号 / 开始时间 --> 结束时间 / 字幕文本 / 空行
    QString normalized = content;
    normalized.replace("\r\n", "\n").replace("\r", "\n");
    QStringList blocks = normalized.split("\n\n", Qt::SkipEmptyParts);

    for (const QString &block : blocks) {
        QStringList lines = block.split("\n", Qt::SkipEmptyParts);
        if (lines.size() < 2) continue;

        // 找到时间轴行（包含 -->）
        int timeLineIdx = -1;
        for (int i = 0; i < lines.size(); i++) {
            if (lines[i].contains("-->")) {
                timeLineIdx = i;
                break;
            }
        }
        if (timeLineIdx < 0) continue;

        QStringList times = lines[timeLineIdx].split("-->");
        if (times.size() < 2) continue;

        double start = parseTimestamp(times[0].trimmed());
        double end = parseTimestamp(times[1].trimmed());
        if (start < 0 || end < 0) continue;

        // 文本部分（时间轴之后的所有行）
        QString text;
        for (int i = timeLineIdx + 1; i < lines.size(); i++) {
            if (!text.isEmpty()) text += "\n";
            text += lines[i];
        }
        // 去除SRT格式标签（如 <i>、<b>）
        text.remove(QRegularExpression("<[^>]+>"));

        if (!text.isEmpty()) {
            m_cues.append({start, end, text});
        }
    }

    return !m_cues.isEmpty();
}

bool SubtitleParser::parseASS(const QString &content)
{
    // ASS格式：[Events] 段中的 Dialogue 行
    QString normalized = content;
    normalized.replace("\r\n", "\n").replace("\r", "\n");
    QStringList lines = normalized.split("\n");

    bool inEvents = false;
    QStringList formatFields;

    for (const QString &rawLine : lines) {
        QString line = rawLine.trimmed();
        if (line.startsWith("[") && line.endsWith("]")) {
            inEvents = (line.toLower() == "[events]");
            continue;
        }
        if (!inEvents) continue;

        if (line.toLower().startsWith("format:")) {
            QString formatStr = line.mid(7).trimmed();
            formatFields = formatStr.split(",");
            for (QString &f : formatFields) f = f.trimmed();
            continue;
        }

        if (line.toLower().startsWith("dialogue:")) {
            QString dialogStr = line.mid(9).trimmed();
            QStringList fields = dialogStr.split(",", Qt::KeepEmptyParts);
            // ASS的Text字段可能包含逗号，需要合并
            if (formatFields.isEmpty()) {
                // 默认格式: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
                formatFields = {"Layer","Start","End","Style","Name","MarginL","MarginR","MarginV","Effect","Text"};
            }

            int startIdx = -1, endIdx = -1, textIdx = -1;
            for (int i = 0; i < formatFields.size(); i++) {
                QString f = formatFields[i].toLower();
                if (f == "start") startIdx = i;
                else if (f == "end") endIdx = i;
                else if (f == "text") textIdx = i;
            }

            if (startIdx < 0 || endIdx < 0 || textIdx < 0) continue;
            if (fields.size() <= textIdx) continue;

            double start = parseASSTimestamp(fields[startIdx].trimmed());
            double end = parseASSTimestamp(fields[endIdx].trimmed());
            if (start < 0 || end < 0) continue;

            // Text字段可能被逗号分割，需要合并textIdx之后的所有部分
            QString text;
            for (int i = textIdx; i < fields.size(); i++) {
                if (i > textIdx) text += ",";
                text += fields[i];
            }
            // 去除ASS覆盖标签（如 {\an8}、{\pos(...)}）
            text.remove(QRegularExpression("\\{[^}]*\\}"));
            text = text.replace("\\N", "\n").replace("\\n", "\n");

            if (!text.isEmpty()) {
                m_cues.append({start, end, text});
            }
        }
    }

    return !m_cues.isEmpty();
}

double SubtitleParser::parseTimestamp(const QString &ts) const
{
    // SRT格式: HH:MM:SS,mmm 或 HH:MM:SS.mmm
    QString t = ts.trimmed();
    t.replace(",", ".");
    QRegularExpression re("(\\d+):(\\d+):(\\d+)[.](\\d+)");
    QRegularExpressionMatch m = re.match(t);
    if (!m.hasMatch()) return -1;
    int h = m.captured(1).toInt();
    int min = m.captured(2).toInt();
    int s = m.captured(3).toInt();
    int ms = m.captured(4).toInt();
    return h * 3600.0 + min * 60.0 + s + ms / 1000.0;
}

double SubtitleParser::parseASSTimestamp(const QString &ts) const
{
    // ASS格式: H:MM:SS.cc
    QString t = ts.trimmed();
    QRegularExpression re("(\\d+):(\\d+):(\\d+)[.](\\d+)");
    QRegularExpressionMatch m = re.match(t);
    if (!m.hasMatch()) return -1;
    int h = m.captured(1).toInt();
    int min = m.captured(2).toInt();
    int s = m.captured(3).toInt();
    int cs = m.captured(4).toInt();
    return h * 3600.0 + min * 60.0 + s + cs / 100.0;
}
