# MediaPlayer 开发大纲（ROADMAP v4）

> ⚠️ 历史文档（2026-10-02，M4 完成时点）。现行计划见仓库根目录 `ROADMAP.md`（v5）。
> 保留原因：记录当时的 T4.8 方案调研、M4 关键决策与踩坑。

> 基于策略2：所有视频统一走稳固基础框架保证流畅；性能足够的电脑可选全方位无损播放，性能不足可降质量保流畅。
> 更新：2026-10-02（M4 完成，全部核心功能实现，项目进入优化完善阶段）
> 前置版本：`ROADMAP_v3.md`（M3 完成）、`ROADMAP_v2.md`（M2 完成）、`ROADMAP.md`（v1）

---

## 产品定位

**目标**：桌面视频播放器，接近无损的原帧率、原音质播放。
**策略**：可配置质量（硬件解码开关 + 分辨率选项），两档并存，手动切换（不做自动降画质）。
**UI 架构**：主窗口纯视频（D3D11 渲染）+ 独立 UI 窗口（设置栏+控制栏+播放列表面板），避免混合渲染冲突。

---

## 里程碑总览

| 里程碑 | 主题 | 状态 |
|--------|------|------|
| M1 | 基础播放框架（三线程 + 软解 + SDL 音频 + Qt 渲染） | ✅ 完成 |
| M2 | 硬件解码 + D3D11 零拷贝渲染（无损视频核心） | ✅ 完成 |
| M3 | 交互完善（seek / 音量 / 全屏 / 配置持久化 / 快捷键 / UI架构重构） | ✅ 完成 |
| M4 | 高级功能（软解回退 / 发布打包 / 高帧率 / 字幕 / 播放列表 / 截图 / 拖拽 / AI翻译字幕） | 🔄 T4.1-T4.7完成，T4.8进行中 |
| M5 | 优化完善（稳定性 / 性能 / UI美化 / 格式兼容） | ⏳ 可选后续阶段 |

---

## M4：已完成总结

### 任务完成清单

| 任务 | 描述 | 关键实现 |
|------|------|----------|
| T4.1 | 软解回退 | sws_scale转NV12 → 动态纹理CPU写入 → CopySubresourceRegion到中转纹理 |
| T4.2 | 发布打包 | WIN32_EXECUTABLE仅Release + QT_NO_DEBUG_OUTPUT，无控制台窗口 |
| T4.3 | 高帧率 | 显示定时器1ms PreciseTimer + Present(0,0)关闭垂直同步 |
| T4.4 | 字幕 | SubtitleParser(SRT/ASS/SSA) + DraggableLabel可拖动悬浮窗 + 开关与文件选择分离 |
| T4.5 | 播放列表 | 添加/移除/双击播放/自动连播 + 可切换列表面板 |
| T4.6 | 截图 | D3D11 back buffer回读 → staging → Map → QImage(RGBA8888) → picture文件夹 |
| T4.7 | 拖拽打开 | window/central/videoWidget三处安装eventFilter + setAcceptDrops |
| T4.8 | 离线AI翻译字幕 | 目标：几乎实时的离线多语言语音识别+翻译字幕（进行中，方案见下文） |
| T4.9 | 高效反馈系统 | 结构化日志 + 崩溃捕获 + 实时诊断面板 + 一键导出诊断包（进行中，方案见 T4.9_DIAGNOSTICS_PLAN.md） |

### M4 关键技术决策

1. **软解回退用动态纹理做中介**：D3D11_USAGE_DYNAMIC + CPU_WRITE，与硬解路径统一走中转纹理池
2. **字幕用独立悬浮窗**：避免被 D3D11 视频窗口裁剪，支持任意位置拖动
3. **播放结束三条件检测**：demux结束 + 视频队列空 + 音频队列空，确保音视频都完毕才切换
4. **截图用 Format_RGBA8888**：与 swap chain 的 R8G8B8A8 格式字节顺序一致（用ARGB32会R/B颠倒）
5. **拖拽三处安装过滤器**：createWindowContainer 容器会拦截事件，必须在所有层级启用

### M4 遇到的典型问题与解决

| 问题 | 根因 | 解决方案 |
|------|------|----------|
| 截图色调偏蓝紫 | swap chain是RGBA，QImage::Format_ARGB32小端是BGRA | 改用 Format_RGBA8888 |
| 拖拽松开无反应 | createWindowContainer容器拦截拖拽事件 | central+videoWidget也安装过滤器并启用拖拽 |
| seek后末尾卡顿 | 测试视频关键帧间隔过长，需解码大量帧追赶 | PTS检查移到纹理拷贝之前加速跳过；接受为视频特性 |
| 播放列表展开UI高度不够 | positionUIWindow用固定UI_TOTAL_HEIGHT | 动态计算playlistPanel可见时增加高度 |

---

## T4.8：离线AI翻译字幕（进行中）

### 目标
几乎实时的离线多语言语音识别 + 翻译字幕，不依赖网络，全部本地推理。

### 最终架构（目标形态）
```
正在播放的音频（从解码管线取PCM）
    │
    ├─ 环形缓冲 + VAD语音活动检测（句子结束触发）
    │     │
    │     └─ 2-5秒语音段 → 流式ASR（原文+时间戳）
    │           │
    │           └─ 离线翻译模型 → 译文
    │                 │
    │                 └─ 字幕显示（延迟目标3-5秒）
```

### 实现路径全罗列

#### 路径A：ASR + 机器翻译 两阶段串联
```
音频 → ASR(原文) → MT(译文) → 显示
```
| 组件 | 可选方案 | 特点 |
|------|----------|------|
| ASR | **Whisper.cpp** | 99语言，C/C++，流式支持，模型75MB-3GB，最成熟 |
| ASR | FunASR (阿里) | 中文优化好，Paraformer模型，需ONNX Runtime |
| ASR | Vosk | 轻量，离线，但多语言支持弱于Whisper |
| ASR | PaddleSpeech | 百度飞桨，中文好，但C++部署复杂 |
| MT | **Argos Translate** | OpenNMT后端，CPU快，50-150MB/语言对，20+语言 |
| MT | NLLB-200 (Meta) | 200+语言互译，单模型600MB-3GB，distilled版可用 |
| MT | M2M-100 (Meta) | 100语言，比NLLB老，模型较大 |
| MT | OPUS-MT (Helsinki-NLP) | 轻量，语言对多，但质量一般 |
| MT | llama.cpp + 翻译prompt | 用大模型做翻译，质量高但慢，模型大 |

**优点**：模块解耦，可独立替换优化；ASR和MT都有成熟离线方案
**缺点**：两阶段延迟叠加；ASR错误会传播到翻译

#### 路径B：端到端语音翻译（Speech-to-Text Translation）
```
音频 → 直接输出目标语言文本
```
| 方案 | 特点 |
|------|------|
| Whisper `--task translate` | 只能翻译成英文，不满足中/日等目标语言 |
| Meta MMS (Massively Multilingual Speech) | 1100+语言识别+100+语言翻译，模型大 |
| CoVoST 训练模型 | 专门语音翻译数据集，但生态不如Whisper成熟 |
| SpeechMatrix | Meta开源语音翻译，支持多语言，但部署复杂 |

**优点**：单阶段延迟低，避免ASR错误传播
**缺点**：模型选择少，多语言互译支持有限，生态不如两阶段成熟

#### 路径C：大模型端到端（音频理解模型）
```
音频 → Qwen2-Audio / Llama 3 Audio → 翻译后文本
```
| 方案 | 特点 |
|------|------|
| Qwen2-Audio | 阿里开源音频理解大模型，支持多语言，可做翻译 |
| Llama 3 Audio | Meta音频理解模型 |
| GPT-4o 级别模型 | 质量最高，但本地部署需要高端GPU |

**优点**：质量最高，可理解上下文，翻译自然
**缺点**：模型巨大（7B+），需要高端GPU，延迟高，不适合实时

### 推理框架选择
| 框架 | 适用模型 | 特点 |
|------|----------|------|
| **whisper.cpp** | Whisper系列 | C/C++原生，GGUF量化，CPU/GPU(Vulkan/CUDA/Metal) |
| **llama.cpp** | LLM系列 | 同上，可用于翻译大模型 |
| ONNX Runtime | FunASR/NLLB/Argos | 跨平台，CPU优化好，GPU需额外包 |
| CTranslate2 | NLLB/OPUS-MT | 专为翻译优化，CPU/GPU都快 |
| OpenVINO | Intel CPU优化 | Intel CPU上推理快 |

### 实时性策略（三种模式）
| 模式 | 延迟 | 实现难度 | 说明 |
|------|------|----------|------|
| 离线完整识别 | 播放前/后台完整识别 | 低 | 先做这个验证效果 |
| VAD分段准实时 | 5-10秒 | 中 | 每句话结束后识别翻译，延迟直播模式 |
| 真正流式 | 2-4秒 | 高 | 流式Whisper+前缀翻译，边说边出结果 |

### 推荐实施路径
1. **Phase 1**：路径A + Whisper.cpp(base) + Argos Translate，离线完整识别翻译（验证效果）
2. **Phase 2**：VAD分段准实时（延迟5-10秒）
3. **Phase 3**：流式Whisper + GPU推理 + 翻译量化，目标延迟3-5秒

### 关键风险
- Whisper流式识别半句准确率低，需要"初步结果→修正"机制
- 翻译需要上下文（最近2-3句），单句翻译可能不准
- GPU部署复杂度高，CPU实时性可能不够，需降级方案
- 模型体积：base(142MB) + 翻译(~300MB) ≈ 500MB额外体积

---

## M5：优化完善（可选后续阶段）

> M1-M4 已完成播放器全部核心功能。以下为可选的优化方向，根据实际需求决定是否实施。

### T5.1 稳定性加固
- 异常文件容错（损坏文件、不支持格式不崩溃）
- 内存泄漏排查（长时间播放/切换视频后的资源释放）
- 线程安全审计（所有共享数据访问加锁验证）

### T5.2 性能优化
- 软解路径性能优化（多线程解码、SIMD）
- 启动速度优化（延迟初始化非必要模块）
- 内存占用优化（队列大小动态调整）

### T5.3 UI 美化
- 主题切换（深色/浅色）
- 控制栏自定义布局
- 字幕样式自定义（字体/颜色/大小/描边）
- 播放列表拖拽排序

### T5.4 格式兼容扩展
- 更多封装格式支持（TS、M2TS、VOB等）
- 音频轨道切换（多音轨视频）
- 章节支持（MKV章节）
- HDR 视频支持（10bit HEVC）

### T5.5 高级功能
- 倍速播放（0.5x-2x，音频变速不变调）
- 画面旋转/翻转
- 音频延迟调节
- 视频滤镜（亮度/对比度/饱和度）

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
- 音频：采样率/位深/声道与源一致，无重采样 → 样本级无损

---

## 架构演进原则

1. **三线程架构不动** — 已证明正确，所有新功能在其上加
2. **独立 UI 窗口不动** — 已证明是 Qt + D3D11 共存的唯一稳定方案
3. **配置注入不硬编码** — 新功能先加 setter 参数，再连 UI
4. **失败必须回退** — 硬件失败→软解，零拷贝失败→CPU 上传，无损音频失败→重采样
5. **每个里程碑独立可运行** — 做完即编译测试，不堆积
6. **D3D11 资源必须用池管理** — 纹理/缓冲区避免每帧分配释放
7. **D3D11 设备全局复用** — 切换视频不重建设备，避免跨设备纹理闪退
8. **分辨率纯手动切换** — 不做自动降画质，避免播放中莫名其妙降画质

---

## 当前代码关键入口

| 功能 | 位置 |
|------|------|
| 读包线程 | `PlayerController.cpp: demuxLoop()` |
| 音频解码线程 | `PlayerController.cpp: audioDecodeLoop()`（含软件音量缩放） |
| 视频解码线程 | `PlayerController.cpp: videoDecodeLoop()`（D3D11VA + 软解回退 + 中转纹理拷贝） |
| D3D11VA 设备初始化 | `PlayerController.cpp: openCodec()`（自建设备 + lock回调 + 全局复用） |
| 音频初始化（无损路径） | `PlayerController.cpp: initAudio()` |
| seek 实现 | `PlayerController.cpp: seek()` |
| 帧显示同步 | `PlayerController.cpp: displayFrame()`（音频时钟插值 + PTS 匹配 + 播放结束检测） |
| 播放列表管理 | `PlayerController.cpp: setPlaylist/addToPlaylist/playIndex/playNext` |
| 软解上传 | `PlayerController.cpp: ensureSwUploadTexture/uploadSoftwareFrame` |
| D3D11 渲染器 | `D3D11Renderer.h/cpp`（交换链/着色器/中转纹理池/截图回读） |
| 中转纹理池 | `D3D11Renderer.cpp: StagingTexturePool` |
| 视频窗口 | `VideoWidget.h/cpp`（QWindow + GDI黑色背景 + 截图接口） |
| 字幕解析 | `SubtitleParser.h/cpp`（SRT/ASS/SSA） |
| 主窗口 + 独立UI窗口 | `main.cpp`（主窗口纯视频 + uiWindow + 全屏 + 字幕悬浮窗 + 播放列表 + 截图 + 拖拽） |
| 配置入口 | `PlayerController::setHardwareDecode() / setOutputResolution() / setVolume()` |
| 全局 D3D11 锁 | `D3D11Renderer.cpp: g_d3d11Mutex` |

---

## 已知限制

1. 仅 Windows 平台（D3D11 绑定），跨平台需替换渲染后端
2. 个别关键帧间隔过长的视频，seek 到末尾几秒恢复较慢（视频本身特性）
3. 软解回退路径已实现但未充分端到端验证
4. 字幕样式暂不支持自定义（字体/颜色/描边）
5. 不支持倍速播放、音轨切换、HDR 等高级功能（M5 可选）
