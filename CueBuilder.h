#pragma once

#include <QString>
#include <QVector>

#include "WhisperTranscriber.h"

// 断句 + 时间轴分配：纯函数，不碰解码/模型，单测可以直接喂假数据。
// 预翻译产物和后续"导入外部 SRT 重排"都走这里，改规则只改一处。
namespace cuebuild {

struct Cue
{
    double start = 0.0;
    double end = 0.0;
    QString text;
};

QVector<Cue> buildCues(const QVector<TranscriptSegment> &segments, int maxChars, double minDuration,
                       double charsPerSecond);

} // namespace cuebuild
