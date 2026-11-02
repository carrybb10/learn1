#include "CueBuilder.h"

#include <QRegularExpression>
#include <QStringList>

namespace cuebuild {

using Cue = cuebuild::Cue;

QVector<Cue> buildCues(const QVector<TranscriptSegment> &segments, int maxChars, double minDuration, double charsPerSecond)
{
    static const QRegularExpression sentenceEnd(QStringLiteral("[。！？!?…]+"));
    static const QRegularExpression clauseEnd(QStringLiteral("[，、；：,;:]+"));
    QVector<Cue> cues;

    auto splitKeepDelimiter = [](const QString &text, const QRegularExpression &re) {
        QStringList parts;
        int start = 0;
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            const int end = m.capturedEnd();
            parts.append(text.mid(start, end - start));
            start = end;
        }
        if (start < text.size())
            parts.append(text.mid(start));
        return parts;
    };

    // whisper 输出的是"呼吸段"，不是句子：间隔小且不超长的相邻段先拼起来
    QVector<TranscriptSegment> merged;
    for (const TranscriptSegment &segment : segments) {
        const QString text = segment.text.trimmed();
        if (text.isEmpty())
            continue;
        if (!merged.isEmpty() && segment.start - merged.last().end <= 0.5
            && merged.last().text.size() + text.size() <= maxChars) {
            merged.last().text += text;
            merged.last().end = segment.end;
            continue;
        }
        merged.append({ segment.start, segment.end, text });
    }

    for (const TranscriptSegment &segment : merged) {
        QString text = segment.text.trimmed();
        if (text.isEmpty())
            continue;

        // 步骤 1+2+3：切成不超过 maxChars 的片段
        QStringList pieces;
        const QStringList sentences = splitKeepDelimiter(text, sentenceEnd);
        for (const QString &sentence : sentences) {
            if (sentence.size() <= maxChars) {
                pieces.append(sentence);
                continue;
            }
            for (const QString &clause : splitKeepDelimiter(sentence, clauseEnd)) {
                if (clause.size() <= maxChars) {
                    pieces.append(clause);
                    continue;
                }
                for (int i = 0; i < clause.size(); i += maxChars)
                    pieces.append(clause.mid(i, maxChars));
            }
        }

        // 步骤 4：按字数比例分配时间
        int totalChars = 0;
        for (const QString &piece : pieces)
            totalChars += piece.size();
        if (totalChars == 0)
            continue;

        // 每条时长由字数按语速估算，段首对齐；累计不超过段尾，剩余时间自然成为句间间隙。
        const double segmentEnd = qMax(0.0, segment.end);
        const double maxCueDuration = 8.0;
        double cursor = segment.start;
        const double span = qMax(minDuration, segmentEnd - segment.start);
        for (const QString &piece : pieces) {
            // 按字数占比分配整段时间：只按语速估算会让字幕比人声早消失，看着就是"不同步"
            const double byChars = span * static_cast<double>(piece.trimmed().size()) / totalChars;
            const double estimated = (static_cast<double>(piece.trimmed().size()) / charsPerSecond);
            const double duration = qBound(minDuration, qMax(byChars, estimated * 0.5), maxCueDuration);
            Cue cue;
            cue.start = cursor;
            cue.end = qMin(cursor + duration, segmentEnd);
            cue.text = piece.trimmed();
            cursor = cue.end;
            if (cue.text.isEmpty())
                continue;
            if (!cues.isEmpty() && cue.end - cue.start < minDuration && cues.last().end >= cue.start - 1e-3) {
                cues.last().text += cue.text;
                cues.last().end = cue.end;
                continue;
            }
            cues.append(cue);
        }
    }
    return cues;
}

} // namespace cuebuild
