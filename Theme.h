#pragma once
// 设计 token（Layer 0 起所有 UI 从这里取值，禁止在业务代码里写死魔法数字）
#include <QColor>

namespace Theme {

// 间距（4px 基础网格）
inline constexpr int SpacingXS = 4;
inline constexpr int SpacingS  = 8;
inline constexpr int SpacingM  = 12;
inline constexpr int SpacingL  = 16;
inline constexpr int SpacingXL = 24;
inline constexpr int SpacingXXL = 32;

// 圆角
inline constexpr int RadiusControl = 4;  // 按钮/输入框等页面内控件
inline constexpr int RadiusOverlay = 8;  // 面板/浮层/弹窗

// 控件高度
inline constexpr int HeightCompact = 28;
inline constexpr int HeightNormal  = 32;
inline constexpr int HeightPrimary = 40;

// 字号
inline constexpr int FontAux     = 12;
inline constexpr int FontBody    = 13;
inline constexpr int FontEmph    = 15;
inline constexpr int FontTitle   = 17;
inline constexpr int FontDisplay = 22;

// 文字层级（不透明度）
inline constexpr double OpacityPrimary   = 1.00;
inline constexpr double OpacitySecondary = 0.70;
inline constexpr double OpacityTertiary  = 0.50;

// 命中区最小尺寸
inline constexpr int MinHitTarget = 32;

// 配色（第一套：深色。三套预设后续通过换表实现）
namespace Dark {
inline const QColor Surface       = QColor(30, 30, 30);
inline const QColor SurfaceAlt    = QColor(40, 40, 40);
inline const QColor SurfaceRaised = QColor(48, 48, 48);
inline const QColor Border        = QColor(60, 60, 60);
inline const QColor TextPrimary   = QColor(235, 235, 235);
inline const QColor TextSecondary = QColor(170, 170, 170);
inline const QColor TextTertiary  = QColor(120, 120, 120);
inline const QColor Accent        = QColor(0, 140, 220);
inline const QColor AccentHover   = QColor(0, 165, 255);
inline const QColor Danger        = QColor(214, 69, 69);
}  // namespace Dark

}  // namespace Theme
