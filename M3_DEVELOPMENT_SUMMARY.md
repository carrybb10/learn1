# M3 阶段开发总结：交互功能与 UI 架构重构

> 完成时间：2026-10-02
> 阶段目标：实现完整的播放器交互功能（seek/音量/全屏/设置持久化/快捷键/末尾处理），并解决 Qt 与 D3D11 混合渲染的 UI 叠加问题

---

## 一、阶段概述

M3 阶段在 M2（D3D11 零拷贝渲染）基础上，实现了播放器的完整交互功能。过程中最大的挑战不是功能本身，而是 **Qt 子控件与 D3D11 原生窗口（createWindowContainer）混合渲染导致的多层叠加、重影、白屏问题**。最终通过**独立 UI 窗口方案**彻底解决，并顺带解决了全屏白色矩形背景问题。

### 完成清单

| 子任务 | 功能 | 状态 |
|--------|------|------|
| T3.1 | 进度条拖拽 seek | ✅ |
| T3.2 | 音量控制（软件 PCM 缩放） | ✅ |
| T3.3 | 全屏播放（淡入淡出控制栏） | ✅ |
| T3.4 | 设置持久化（QSettings） | ✅ |
| T3.5 | 文件末尾乱码保护 | ✅ |
| T3.6 | 键盘快捷键 | ✅ |
| 附加 | 独立 UI 窗口架构（解决叠加问题） | ✅ |
| 附加 | 全屏无白色矩形（渲染器预初始化+设备匹配） | ✅ |

---

## 二、各子任务实现要点

### T3.1 进度条拖拽 seek

**实现流程**：
1. `sliderPressed` → 标记 `isSeeking = true`，暂停进度条更新
2. `sliderReleased` → 调用 `player->seek(ms)`
3. `seek()` 内部：停止三线程 → `av_seek_frame` 跳转 → `avcodec_flush_buffers` 清空解码器 → 清空所有包/帧队列（归还中转纹理）→ 重启线程

**关键细节**：
- 进度条 range 从秒级改为**毫秒级**，提升拖拽精度
- seek 时必须归还 D3D11 中转纹理池中的纹理，否则 GPU 内存泄漏
- 往前拖拽容易卡住的原因：队列中残留的旧帧 PTS 大于目标位置，导致 displayFrame 等待。解决：seek 时彻底清空帧队列

### T3.2 音量控制

**方案选择**：SDL `SDL_SetAudioDeviceGain` 在当前版本不可用，改用**软件 PCM 缩放**。

**实现**：
- 在 SDL 音频回调中，对每个样本乘以音量系数（0.0 ~ 1.0）
- 支持 S16/FLT/S32 三种格式，带削波保护（FLT 超出 [-1,1] 时钳位）
- 控制栏滑条 0-100，映射到 0.0-1.0
- 音量图标随音量变化（🔇/🔊）

### T3.3 全屏播放

**最终方案**（经历多次迭代，见第三节）：
- 主窗口 `showFullScreen()` / `showNormal()` 切换
- 独立 UI 窗口 2.5 秒无鼠标操作后淡出（`windowOpacity` 动画），鼠标移动淡入
- F/ESC 快捷键切换
- 全屏时先 `hide()` UI 窗口再 `showFullScreen()`，避免 `WindowStaysOnTopHint` 干扰

### T3.4 设置持久化

- `QSettings("MediaPlayer", "Config")` 保存：硬件解码开关、输出分辨率、音量
- 启动时读取并恢复，UI 控件和 PlayerController 同步设置
- 每次修改立即写入（`setValue`），无需手动保存

### T3.5 文件末尾乱码保护

**问题**：文件结束时解码器 flush 出不完整帧，显示为花屏/乱码。

**解决**：
- demux 线程读到 EOF 后标记 `m_demuxFinished = true`
- `displayFrame()` 检测到队列只剩最后 2 帧时停止刷新，保持上一帧画面
- 避免不完整帧被渲染

### T3.6 键盘快捷键

| 按键 | 功能 |
|------|------|
| 空格 | 播放/暂停 |
| ← / → | 后退/前进 5 秒 |
| ↑ / ↓ | 音量 ±5 |
| F | 全屏切换 |
| ESC | 退出全屏 |

使用 `QShortcut` 绑定到主窗口，全局生效。

---

## 三、UI 叠加问题：六次失败与最终方案

这是 M3 阶段最耗时的部分。核心矛盾：**`QWidget::createWindowContainer()` 创建的原生窗口与 Qt 子控件在同一窗口中混合渲染时，Qt 的 backing store 会与 D3D11 交换链冲突**，导致多层叠加、重影、白屏。

### 尝试 1：QGraphicsOpacityEffect 淡入淡出
- 做法：给控制栏添加 `QGraphicsOpacityEffect`，动画改变 opacity
- 结果：与 `createWindowContainer` 原生窗口冲突，导致**白屏**
- 结论：OpacityEffect 会触发 Qt backing store 重绘，覆盖 D3D11 渲染结果

### 尝试 2：maximumHeight 动画
- 做法：动画改变控制栏 `maximumHeight`，实现展开/收起
- 结果：控件未真正隐藏（只是高度为0），导致**多层叠加/重影**
- 结论：高度动画不改变控件的可见性，布局中仍占据绘制区域

### 尝试 3：悬浮在 central widget 上
- 做法：控制栏 `setParent(central)`，用 move 定位，`raise()` 置顶
- 结果：被 `createWindowContainer` 原生窗口覆盖，**白屏**
- 结论：原生窗口层级高于普通 QWidget，无法被覆盖

### 尝试 4：悬浮在 videoWidget 上（setParent）
- 做法：控制栏 parent 设为 `createWindowContainer` 返回的 QWidget
- 结果：程序**卡死**
- 结论：在原生窗口容器上添加子控件会导致事件循环死锁

### 尝试 5：独立控制栏窗口 + 设置栏仍在主窗口
- 做法：控制栏改为独立窗口（`Qt::Tool | FramelessWindowHint | WindowStaysOnTopHint`），设置栏保留在主窗口布局
- 结果：设置栏仍在主窗口中，与 D3D11 窗口混合渲染，**设置栏多层叠加**
- 结论：只要主窗口中有 Qt 子控件，就会与原生窗口冲突

### 最终方案：主窗口纯视频 + 完全独立 UI 窗口
- 做法：主窗口只放 `createWindowContainer` 视频区域，**所有 UI（设置栏+控制栏）移到独立 `uiWindow`**
- `uiWindow` 标志：`Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus`
- parent 设为主窗口（移除 `WindowStaysOnTopHint`，避免切换窗口时置顶）
- 主窗口 resize/move 时通过事件过滤器同步定位 `uiWindow` 到底部
- 全屏时 `uiWindow` 用 `windowOpacity` 动画淡入淡出

**为什么有效**：独立原生窗口有自己的交换链和消息循环，与主窗口的 D3D11 渲染完全隔离，不存在 backing store 冲突。

---

## 四、全屏白色矩形问题排查

### 现象
全屏后视频区域扩展到全屏，但中间出现白色矩形背景。

### 排查过程
1. 添加 `[VideoWidget] resizeEvent` 诊断输出，发现**所有 resizeEvent 都是 `initialized: false`**
2. 原因：`exposeEvent` 之前只在有 pending 视频帧时才调用 `ensureInit()`，无视频时渲染器不初始化
3. 渲染器未初始化 → D3D11 交换链未创建 → QWindow 显示默认**白色背景**
4. 全屏时 `resizeEvent` 的 `initialized: false` 也不会触发交换链 resize

### 解决
1. `exposeEvent` 中**无条件调用 `ensureInit()`**，窗口一显示就初始化渲染器（无视频时创建独立 D3D11 设备）
2. 新问题：渲染器用独立设备初始化后，视频帧来自解码设备，**跨设备纹理无法渲染**（黑屏）
3. 添加 `m_usingDecoderDevice` 标志，视频帧到来时检测设备不匹配，自动释放并重新初始化解码设备
4. `resize()` 后立即清除为黑色并 Present，避免 resize 间隙显示白色

---

## 五、关键技术决策记录

| 问题 | 结论 |
|------|------|
| Qt 子控件与 D3D11 窗口共存 | **不可行**，必须用独立 UI 窗口，主窗口只放视频 |
| QGraphicsOpacityEffect | 与 createWindowContainer 冲突，导致白屏，不可用 |
| maximumHeight 动画 | 控件未真正隐藏，导致多层叠加，不可用 |
| 悬浮子控件在 central/videoWidget 上 | 被原生窗口覆盖或卡死，不可用 |
| 独立 UI 窗口 parent | 设为主窗口，不用 WindowStaysOnTopHint，避免切换窗口时置顶 |
| 全屏时 UI 窗口 | 先 hide 再 showFullScreen，避免干扰主窗口全屏 |
| 渲染器初始化时机 | exposeEvent 中无条件初始化，确保无视频时黑色背景 |
| 渲染器设备匹配 | 独立设备初始化后，视频帧到来时检测并切换到解码设备 |
| D3D11 resize 后 | 立即清除为黑色并 Present，避免白色背景间隙 |
| seek 精度 | 进度条 range 用毫秒级，不用秒级 |
| 音量控制 | SDL_SetAudioDeviceGain 不可用，用软件 PCM 缩放 |

---

## 六、反思与优化

### Token 效率
- **问题**：UI 叠加问题尝试了 6 种方案才找到正确方向，消耗了大量对话轮次
- **优化**：遇到 Qt + 原生窗口混合渲染问题时，应第一时间识别为"backing store 冲突"，直接采用独立窗口方案，而不是逐种尝试
- **教训**：`createWindowContainer` 的文档明确提到原生窗口与 Qt 控件的层级限制，应提前查阅

### 目标契合度
- M3 的核心目标是"交互功能"，但实际耗时最多的是 UI 架构重构
- 这说明 M2 阶段选择 QWindow + D3D11 时，没有充分评估 UI 层的兼容性代价
- **未来项目**：选择渲染方案时，应同时评估 UI 框架的兼容性，避免后期重构

### 代码质量
- 独立 UI 窗口方案引入了窗口定位同步逻辑（事件过滤器），增加了复杂度
- 但这是 Qt + 原生渲染共存的必要代价，架构上是合理的
- 后续可考虑用 QML/QtQuick 重写 UI，从根本上解决混合渲染问题

### 测试覆盖
- M3 阶段主要靠手动测试，缺乏自动化测试
- 全屏/seek/切换视频等场景容易引入回归，后续应考虑添加基本的自动化测试

---

## 七、M3 完成后的项目状态

- **核心播放**：4K 60fps 原分辨率无损播放，音频无损直通 ✅
- **交互**：seek/音量/全屏/快捷键/设置持久化 全部实现 ✅
- **UI 架构**：主窗口纯视频 + 独立 UI 窗口，无叠加/重影 ✅
- **稳定性**：切换视频不闪退，seek 不卡死 ✅
- **下一步**：M4 高级功能（字幕/播放列表/截图/软解回退/发布打包）
