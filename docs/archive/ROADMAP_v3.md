# MediaPlayer 开发大纲（ROADMAP v3）

> 基于策略2：所有视频统一走稳固基础框架保证流畅；性能足够的电脑可选全方位无损播放，性能不足可降质量保流畅。
> 更新：2026-10-02（M3 完成，交互功能全部实现，独立UI窗口架构）
> 前置版本：`ROADMAP_v2.md`（M2 完成）、`ROADMAP.md`（v1，M2 进行中）

---

## 产品定位

**目标**：桌面视频播放器，接近无损的原帧率、原音质播放。
**策略**：可配置质量（硬件解码开关 + 分辨率选项），两档并存，渐进实现。
**UI 架构**：主窗口纯视频（D3D11 渲染）+ 独立 UI 窗口（设置栏+控制栏），避免混合渲染冲突。

---

## 里程碑总览

| 里程碑 | 主题 | 状态 |
|--------|------|------|
| M1 | 基础播放框架（三线程 + 软解 + SDL 音频 + Qt 渲染） | ✅ 完成 |
| M2 | 硬件解码 + D3D11 零拷贝渲染（无损视频核心） | ✅ 完成 |
| M3 | 交互完善（seek / 音量 / 全屏 / 配置持久化 / 快捷键 / UI架构重构） | ✅ 完成 |
| M4 | 高级功能（软解回退 / 字幕 / 播放列表 / 截图 / 发布打包） | ⏳ 未开始 |

---

## M3：已完成总结

### 最终方案
- **UI 架构**：主窗口纯视频（createWindowContainer）+ 独立 uiWindow（设置栏+控制栏），悬浮在主窗口底部
- **全屏**：F/ESC 切换，独立 UI 窗口 windowOpacity 淡入淡出，2.5s 无操作自动隐藏
- **seek**：停止线程 → av_seek_frame + flush → 清空队列（归还纹理）→ 重启线程，毫秒级精度
- **音量**：软件 PCM 缩放（S16/FLT/S32，带削波保护），SDL_SetAudioDeviceGain 不可用
- **设置持久化**：QSettings 保存硬件解码/分辨率/音量
- **末尾保护**：demux 结束标记，队列剩最后2帧停止刷新

### UI 叠加问题关键决策（避免重复踩坑）

| 尝试方案 | 结果 |
|----------|------|
| QGraphicsOpacityEffect 淡入淡出 | 与 createWindowContainer 冲突，白屏 |
| maximumHeight 动画 | 控件未真正隐藏，多层叠加/重影 |
| 悬浮在 central widget 上 | 被原生窗口覆盖，白屏 |
| 悬浮在 videoWidget 上 | 程序卡死 |
| 独立控制栏 + 设置栏在主窗口 | 设置栏仍与D3D11窗口冲突，叠加 |
| **主窗口纯视频 + 完全独立UI窗口** | **✅ 最终方案** |

### 全屏白色矩形关键决策
- exposeEvent 必须无条件初始化渲染器（否则无视频时白色背景，全屏resize不触发）
- 渲染器用独立设备初始化后，视频帧到来时必须检测设备不匹配并重新初始化解码设备
- resize 后立即清除为黑色并 Present

---

## M4：高级功能（下一阶段）

### T4.1 软件解码回退路径适配 D3D11 渲染
- 当前：硬解失败会无画面
- 目标：软解 YUV 帧上传到 D3D11 纹理，走同一渲染管线
- 实现：创建动态 D3D11 纹理，用 UpdateSubresource 上传 YUV 数据，着色器复用

### T4.2 发布版本打包
- 去掉 qDebug 控制台输出（CMAKE_BUILD_TYPE Release 时定义 QT_NO_DEBUG_OUTPUT）
- 设置 Windows 子系统（WIN32_EXECUTABLE），不显示控制台窗口
- windeployqt 自动部署 Qt 依赖
- 考虑 Inno Setup 或简单 zip 打包

### T4.3 高帧率视频支持
- 当前：Present(1,0) 垂直同步限制到显示器刷新率
- 目标：Present(0,0) 关闭垂直同步，支持 120/144/240fps
- 注意：关闭垂直同步可能导致画面撕裂，可作为可选设置

### T4.4 字幕支持
- 支持 SRT/ASS 格式
- 用 libass 渲染 ASS 字幕，或自己实现 SRT 解析
- 字幕叠加在 D3D11 渲染管线中（纹理混合）

### T4.5 播放列表 / 连播
- 支持拖入多个文件
- 自动连播（当前文件结束后播放下一个）
- 播放列表面板（独立窗口或侧边栏）

### T4.6 截图功能
- 当前帧导出 PNG
- 从 D3D11 纹理回读（ID3D11DeviceContext::Map）到 CPU，保存为图片

### T4.7 拖拽打开文件
- 主窗口接受文件拖拽（dragEnterEvent / dropEvent）
- 支持多文件拖入（加入播放列表）

---

## 无损目标技术路径（最终形态，核心已达成）

```
源文件
  │
  ├─ 视频：demux → D3D11VA 硬件解码 → GPU内拷贝到中转纹理
  │        → D3D11 着色器（NV12→RGB BT.709）→ 交换链显示
  │        （零 CPU 回传、零 sws_scale，帧率 1:1）✅ 已达成
  │
  └─ 音频：demux → 解码（源采样率/格式/声道）
           → planar→packed 纯拷贝（无重采样）
           → 软件音量缩放（无损路径下音量=100%时直通）
           → SDL 直通输出（采样率匹配时无 SRC）✅ 已达成
```

**无损判定标准**：
- 视频：解码帧 GPU 内拷贝后直接渲染，无 CPU 回传、无格式转换 → 帧级无损 ✅
- 音频：采样率/位深/声道与源一致，无重采样 → 样本级无损（SDL shared 模式理论有 SRC 风险，WASAPI exclusive 才绝对保证）

---

## 架构演进原则

1. **三线程架构不动** — 已证明正确，所有新功能在其上加
2. **独立 UI 窗口不动** — 已证明是 Qt + D3D11 共存的唯一稳定方案
3. **配置注入不硬编码** — 新功能先加 setter 参数，再连 UI
4. **失败必须回退** — 硬件失败→软解（T4.1），零拷贝失败→CPU 上传，无损音频失败→重采样
5. **每个里程碑独立可运行** — 做完即编译测试，不堆积
6. **D3D11 资源必须用池管理** — 纹理/缓冲区避免每帧分配释放
7. **D3D11 设备全局复用** — 切换视频不重建设备，避免跨设备纹理闪退

---

## 当前代码关键入口

| 功能 | 位置 |
|------|------|
| 读包线程 | `PlayerController.cpp: demuxLoop()` |
| 音频解码线程 | `PlayerController.cpp: audioDecodeLoop()`（含软件音量缩放） |
| 视频解码线程 | `PlayerController.cpp: videoDecodeLoop()`（D3D11VA + 中转纹理拷贝） |
| D3D11VA 设备初始化 | `PlayerController.cpp: openCodec()`（自建设备 + lock回调 + 全局复用） |
| 音频初始化（无损路径） | `PlayerController.cpp: initAudio()` |
| seek 实现 | `PlayerController.cpp: seek()` |
| 帧显示同步 | `PlayerController.cpp: displayFrame()`（音频时钟插值 + PTS 匹配 + 末尾保护） |
| D3D11 渲染器 | `D3D11Renderer.h/cpp`（交换链/着色器/中转纹理池/resize后黑色清除） |
| 中转纹理池 | `D3D11Renderer.cpp: StagingTexturePool` |
| 视频窗口 | `VideoWidget.h/cpp`（QWindow + expose预初始化 + 设备匹配检测） |
| 主窗口 + 独立UI窗口 | `main.cpp`（主窗口纯视频 + uiWindow 设置栏+控制栏 + 全屏逻辑） |
| 配置入口 | `PlayerController::setHardwareDecode() / setOutputResolution() / setVolume()` |
| 全局 D3D11 锁 | `D3D11Renderer.cpp: g_d3d11Mutex` |

---

## 已知限制

1. 仅 Windows 平台（D3D11 绑定），跨平台需替换渲染后端
2. 软件解码回退路径暂未适配 D3D11 渲染（T4.1）
3. 垂直同步限制最高帧率为显示器刷新率（T4.3）
4. 控制台窗口显示诊断输出（T4.2 发布时去掉）
5. 无字幕/播放列表/截图/拖拽（T4.4-T4.7）
