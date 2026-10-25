# MediaPlayer 开发大纲（ROADMAP）

> 基于策略2：所有视频统一走稳固基础框架保证流畅；性能足够的电脑可选全方位无损播放，性能不足可降质量保流畅。
> 更新：2026-10-01

---

## 产品定位

**目标**：桌面视频播放器，接近无损的原帧率、原音质播放。
**策略**：可配置质量（硬件解码开关 + 分辨率选项），两档并存，渐进实现。

---

## 里程碑总览

| 里程碑 | 主题 | 状态 |
|--------|------|------|
| M1 | 基础播放框架（三线程 + 软解 + SDL 音频 + Qt 渲染） | ✅ 完成 |
| M2 | 硬件解码 + 零拷贝渲染（无损视频核心） | 🔄 进行中（DXVA2 完成，D3D11 未开始） |
| M3 | 交互完善（seek / 音量 / 全屏 / 配置持久化） | ⏳ 未开始 |
| M4 | 高级功能（字幕 / 播放列表 / 截图 / 跨平台） | ⏳ 未开始 |

---

## M2：硬件解码 + 零拷贝渲染（当前）

### 已完成
- [x] DXVA2 硬件解码（`av_hwdevice_ctx_create` + `hw_device_ctx`）
- [x] 硬件帧回传 CPU（`av_hwframe_transfer_data`），回传失败跳过该帧
- [x] 动态 swsCtx（按实际帧格式重建，解决黑屏）
- [x] 硬件解码失败自动回退软件解码
- [x] 硬件解码开关（自动/关闭）

### 待完成

#### T2.1 D3D11 零拷贝渲染（无损视频核心，优先级最高）
- **目标**：DXVA2 解码帧直接在 GPU 上显示，去掉 `av_hwframe_transfer_data` + `sws_scale` + QImage 回传链路
- **方案A（推荐）**：Qt 6 的 `QRhi`（RHI 抽象层）— 官方支持，与 QtWidgets 集成好，学习成本中等
  - `QRhi` + `QRhiTexture`（DXVA2 帧 → D3D11 纹理 → shader 转换 YUV→RGB → 交换链）
- **方案B**：`QWindow` + 原生 D3D11 — 性能最优，但需自行管理交换链/纹理共享/窗口 resize，工作量最大
- **方案C**：`QOpenGLWidget` + DXVA2→D3D11 共享纹理 + OpenGL interop — 兼容性好但链路复杂
- **验收**：4K 60fps 原分辨率播放，CPU 占用 <20%，无回传开销，帧率 60fps 稳定
- **注意**：DXVA2 解码帧是 D3D9 纹理，与 D3D11 渲染需要互操作（`ID3D11Device` 通过 D3D9 共享句柄 `ID3D11Texture2D` 打开），或考虑切换解码器到 D3D11VA（`AV_HWDEVICE_TYPE_D3D11VA`），直接用 D3D11 纹理避免互操作

#### T2.2 解码器升级 D3D11VA（可选，与 T2.1 联动）
- FFmpeg 支持 `AV_HWDEVICE_TYPE_D3D11VA`，解码帧直接是 D3D11 纹理，省去 D3D9→D3D11 互操作
- Windows 10+ 推荐；DXVA2 作为兼容回退

#### T2.3 渲染方式自动探测
- GPU 支持 D3D11 → 零拷贝渲染；否则 → QImage 回传渲染（当前实现兜底）
- 与策略2一致：高端机零拷贝无损，低端机回传降分辨率

---

## M3：交互完善

- [ ] **T3.1 进度条拖拽 seek**：`av_seek_frame` + 清空所有队列 + 解码器 `avcodec_flush_buffers` + 重新从目标 PTS 播放；拖动中暂停更新、松手跳转
- [ ] **T3.2 音量控制**：SDL `SDL_SetAudioDeviceGain`（或音量滑条映射）
- [ ] **T3.3 全屏播放**：`QWidget` 全屏 + 自动隐藏控制栏（鼠标移动显示）
- [ ] **T3.4 设置持久化**：`QSettings` 保存硬件解码/分辨率选项，启动时恢复
- [ ] **T3.5 文件末尾乱码处理**：文件结束时 flush 解码器、丢弃末尾不完整帧
- [ ] **T3.6 键盘快捷键**：空格播放/暂停、←→ seek、↑↓ 音量、F 全屏

---

## M4：高级功能

- [ ] 字幕支持（ASS/SRT，libass）
- [ ] 播放列表（拖入多个文件自动连播）
- [ ] 截图功能（当前帧导出 PNG）
- [ ] 拖拽打开文件
- [ ] 记忆上次播放位置
- [ ] 跨平台编译（Linux/macOS，音频后端换 PulseAudio/CoreAudio）
- [ ] 音频 WASAPI exclusive 模式（bit-perfect 终极方案）

---

## 无损目标技术路径（最终形态）

```
源文件
  │
  ├─ 视频：demux → DXVA2/D3D11VA 硬件解码 → D3D11 纹理
  │        → GPU shader（YUV→RGB，高质量 BT.709）→ 交换链显示
  │        （零回传、零 sws_scale，帧率 1:1）
  │
  └─ 音频：demux → 解码（源采样率/格式/声道）
           → planar→packed 纯拷贝（无重采样）
           → SDL/WASAPI 直通输出（采样率匹配时无 SRC）
```

**无损判定标准**：
- 视频：解码帧直接 GPU 渲染，无 CPU 回传、无格式转换 → 帧级无损
- 音频：采样率/位深/声道与源一致，无重采样 → 样本级无损（SDL shared 模式理论有 SRC 风险，WASAPI exclusive 才绝对保证）

---

## 架构演进原则

1. **三线程架构不动** — 已证明正确，所有新功能在其上加
2. **配置注入不硬编码** — 新功能先加 setter 参数，再连 UI
3. **失败必须回退** — 硬件失败→软解，零拷贝失败→QImage 渲染，无损音频失败→重采样
4. **每个里程碑独立可运行** — 做完即编译测试，不堆积

---

## 当前代码关键入口

| 功能 | 位置 |
|------|------|
| 读包线程 | `PlayerController.cpp: demuxLoop()` |
| 音频解码线程 | `PlayerController.cpp: audioDecodeLoop()` |
| 视频解码线程 | `PlayerController.cpp: videoDecodeLoop()` |
| 硬件解码初始化 | `PlayerController.cpp: openCodec()` |
| 音频初始化（无损路径） | `PlayerController.cpp: initAudio()` |
| 帧→QImage（回传渲染） | `PlayerController.cpp: videoFrameToQImage()` |
| 配置入口 | `PlayerController::setHardwareDecode() / setOutputResolution()` |
| UI 设置栏 | `main.cpp: settingsBar` |
