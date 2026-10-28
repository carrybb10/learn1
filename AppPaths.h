#ifndef APPPATHS_H
#define APPPATHS_H

#include <QString>
#include <QStringList>

// 模型/资源定位。
// 解决两个问题：
//   1) 原来把模型路径按 `../../third_party/...` 相对 exe 硬拼，换部署形态（便携包/安装版）就失效；
//   2) 用户可能把模型放在任意位置，需要一个不改代码就能指过去的入口。
//
// 查找顺序：便携 ini 配置 > QSettings 显式配置 > 应用目录下的候选相对路径 > 当前工作目录下的候选。
namespace AppPaths {

// 便携配置：指定一个 ini（通常 <exe>/MediaPlayer.ini），键写成 [ai] whisperModel=...
// 便携包解压到任意目录后，用户不用碰注册表就能指向模型位置。传空串关闭。
void setConfigFile(const QString &iniPath);
QString configFile();

// 返回绝对路径；全部候选都不存在时返回空串（调用方负责提示用户放模型）。
// settingsKey 形如 "ai/whisperModel"，值可以是绝对路径，也可以是相对应用目录的路径。
QString resolveResource(const QString &settingsKey, const QStringList &relativeCandidates);

// 便携包默认放模型的目录（<exe>/models），用于给用户提示"放哪里"。
QString modelsDirHint();

// 最近一次 resolveResource 尝试过的位置，用于失败时一次性告诉用户找了哪里。
QStringList lastTriedLocations();

// GPU 构建（-DMP_CUDA=ON 时定义 MP_HAVE_CUDA）优先大模型；CPU 构建优先小模型。
bool preferHeavyModels();

// 两个模型在各部署形态下的常见位置（按优先级）
QStringList whisperModelCandidates();
QStringList translatorModelCandidates();
QStringList vadModelCandidates();

} // namespace AppPaths

#endif // APPPATHS_H
