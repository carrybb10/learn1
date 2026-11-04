#pragma once
// 动效时长（ms）。原则：只动 opacity / geometry，不做位置飞入等重特效。
namespace Anim {

inline constexpr int PressMs      = 80;   // 按下反馈
inline constexpr int HoverMs      = 120;  // 悬停变色
inline constexpr int ChromeInMs   = 180;  // 控制条淡入
inline constexpr int ChromeOutMs  = 180;  // 控制条淡出
inline constexpr int PanelMs      = 220;  // 设置面板展开/收起
inline constexpr int ToastMs      = 160;  // Toast 出现
inline constexpr int SplashFadeMs = 180;  // Splash 淡出

// 波浪进度条内部刷新帧率
inline constexpr int WaveRefreshMs = 16;

}  // namespace Anim
