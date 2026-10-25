# MediaPlayer 开发大纲（ROADMAP v2）

> 基于策略2：所有视频统一走稳固基础框架保证流畅；性能足够的电脑可选全方位无损播放，性能不足可降质量保流畅。
> 更新：2026-10-01（M2 完成，D3D11 零拷贝渲染达成）
> 前置版本：`ROADMAP.md`（v1，M2 进行中）

---

## 产品定位

**目标**：桌面视频播放器，接近无损的原帧率、原音质播放。
**策略**：可配置质量（硬件解码开关 + 分辨率选项），两档并存，渐进实现。

---

## 里程碑总览

| 里程碑 | 主题 | 状态 |
|--------|------|------|
| M1 | 基础播放框架（三线程 + 软解 + SDL 音频 + Qt 渲染） | ✅ 完成 |
| M2 | 硬件解码 + D3D11 零拷贝渲染（无损视频核心） | ✅ 完成 |
| M3 | 交互完善（seek / 音量 / 全屏 / 配置持久化 / 快捷键） | ⏳ 未开始 |
| M4 | 高级功能（字幕 / 播放列表 / 截图 / 跨平台） | ⏳ 未开始 |

---

## M2：已完成总结

### 最终方案
- **解码器**：D3D11VA（自创建 D3D11 设备，解码+显示共用，替代 DXVA2）
- **渲染**：QWindow + 原生 D3D11 交换链（替代 QWidget + QPainter）
- **帧传递**：解码线程 GPU 内拷贝到中转纹理池 → 渲染线程直接采样（零 CPU 回传）
- **线程安全**：D3D11VA lock/unlock 回调 + 全局互斥锁

### 关键技术决策记录（避免重复踩坑）

| 问题 | 结论 |
|------|------|
| QWidget + D3D11 交换链 | **不可行**，Qt backing store 会覆盖渲染结果，必须用 QWindow |
| 解码纹理直接创建 SRV | **不可行**，BindFlags 只有 DECODER，必须拷贝到中转纹理 |
| FFmpeg 自动创建设备 | **不支持显示输出**，必须自己创建设备传给 FFmpeg |
| D3D11 默认背面剔除 | 全屏四边形顶点逆时针会被剔除，必须 `CULL_NONE` |
| 解码纹理 AddRef 后入队 | **会被覆盖**，解码器复用纹理数组，必须在解码线程立即拷贝 |
| D3D11 上下文线程安全 | 必须设置 `AVD3D11VADeviceContext.lock/unlock` 回调 |
| 每帧创建中转纹理 | 有性能开销，用纹理池循环使用 |

### 性能验证
- 4K 60fps 89Mbps：decodeFps=60, displayFps=60, frameQueue=9，全程稳定
- 音频无损直通：44100Hz 立体声 flt，无重采样

---

## M3：交互完善（下一阶段）

### T3.1 进度条拖拽 seek
- `av_seek_frame` 跳转到目标位置
- 清空所有包队列和帧队列
- 解码器 `avcodec_flush_buffers`
- 拖动中暂停显示更新，松手跳转
- 注意：D3D11 中转纹理池中的纹理需要归还

### T3.2 音量控制
- SDL `SDL_SetAudioDeviceGain` 或软件音量（PCM 缩放）
- UI 音量滑条

### T3.3 全屏播放
- QWindow 全屏（`setWindowState(Qt::WindowFullScreen)`）
- 自动隐藏控制栏，鼠标移动显示
- ESC 退出全屏

### T3.4 设置持久化
- `QSettings` 保存硬件解码开关、分辨率选项、音量
- 启动时恢复配置

### T3.5 文件末尾处理
- 文件结束时 flush 解码器，丢弃末尾不完整帧
- 自动停止或循环播放选项

### T3.6 键盘快捷键
- 空格：播放/暂停
- ←/→：前后 seek 5 秒
- ↑/↓：音量增减
- F：全屏
- O：打开文件

---

## M4：高级功能

- [ ] 字幕支持（ASS/SRT，libass）
- [ ] 播放列表（拖入多个文件自动连播）
- [ ] 截图功能（当前帧导出 PNG，从 D3D11 纹理回读）
- [ ] 拖拽打开文件
- [ ] 记忆上次播放位置
- [ ] 跨平台编译（Linux/macOS，D3D11 替换为 OpenGL/Vulkan）
- [ ] 音频 WASAPI exclusive 模式（bit-perfect 终极方案）
- [ ] 高帧率视频支持（120/144/240fps，Present(0) 关闭垂直同步）

---

## 无损目标技术路径（最终形态，已达成核心）

```
源文件
  │
  ├─ 视频：demux → D3D11VA 硬件解码 → GPU内拷贝到中转纹理
  │        → D3D11 着色器（NV12→RGB BT.709）→ 交换链显示
  │        （零 CPU 回传、零 sws_scale，帧率 1:1）✅ 已达成
  │
  └─ 音频：demux → 解码（源采样率/格式/声道）
           → planar→packed 纯拷贝（无重采样）
           → SDL 直通输出（采样率匹配时无 SRC）✅ 已达成
```

**无损判定标准**：
- 视频：解码帧 GPU 内拷贝后直接渲染，无 CPU 回传、无格式转换 → 帧级无损 ✅
- 音频：采样率/位深/声道与源一致，无重采样 → 样本级无损（SDL shared 模式理论有 SRC 风险，WASAPI exclusive 才绝对保证）

---

## 架构演进原则

1. **三线程架构不动** — 已证明正确，所有新功能在其上加
2. **配置注入不硬编码** — 新功能先加 setter 参数，再连 UI
3. **失败必须回退** — 硬件失败→软解，零拷贝失败→QImage 渲染（待实现回退），无损音频失败→重采样
4. **每个里程碑独立可运行** — 做完即编译测试，不堆积
5. **D3D11 资源必须用池管理** — 纹理/缓冲区避免每帧分配释放

---

## 当前代码关键入口

| 功能 | 位置 |
|------|------|
| 读包线程 | `PlayerController.cpp: demuxLoop()` |
| 音频解码线程 | `PlayerController.cpp: audioDecodeLoop()` |
| 视频解码线程 | `PlayerController.cpp: videoDecodeLoop()`（D3D11VA + 中转纹理拷贝） |
| D3D11VA 设备初始化 | `PlayerController.cpp: openCodec()`（自建设备 + lock回调） |
| 音频初始化（无损路径） | `PlayerController.cpp: initAudio()` |
| 帧显示同步 | `PlayerController.cpp: displayFrame()`（音频时钟插值 + PTS 匹配） |
| D3D11 渲染器 | `D3D11Renderer.h/cpp`（交换链/着色器/中转纹理池） |
| 中转纹理池 | `D3D11Renderer.cpp: StagingTexturePool` |
| 视频窗口 | `VideoWidget.h/cpp`（QWindow + D3D11 渲染调用） |
| 配置入口 | `PlayerController::setHardwareDecode() / setOutputResolution()` |
| UI 设置栏 | `main.cpp: settingsBar` |
| 全局 D3D11 锁 | `D3D11Renderer.cpp: g_d3d11Mutex` |

---

## 已知限制

1. 仅 Windows 平台（D3D11 绑定），跨平台需替换渲染后端
2. 软件解码回退路径暂未适配 D3D11 渲染（当前硬解失败会无画面，需实现软解→D3D11 纹理上传）
3. 垂直同步限制最高帧率为显示器刷新率（高帧率视频需 Present(0)）
4. 控制台窗口显示诊断输出（发布时需去掉 qDebug 并设置 Windows 子系统）
