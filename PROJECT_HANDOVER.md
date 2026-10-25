# MediaPlayer 项目对接方案（Handover）

> 生成日期：2026-10-25
> 用途：在新对话中无缝衔接项目所需的全部上下文、大纲、环境依赖、数据/日志位置、进度与问题清单
> 项目根目录：`D:\new\Player\`

---

## 一、项目概述（上下文）

**目标**：开发一款基于 Qt6 + FFmpeg + SDL2 + D3D11 的**桌面无损视频播放器**，核心差异化是"离线 AI 多语言翻译字幕"——视频不出本机，本地 Whisper ASR + HY-MT2 翻译模型实时/准实时生成双语字幕。

**产品定位**：
- 接近无损的原帧率、原音质播放（D3D11 零拷贝渲染 + 音频直通）
- 完全离线 AI 字幕（隐私安全，无 API 费用）
- 目标语言：日/英/韩/中互译
- 模型体积约束：<10GB，无麻烦部署环境

**架构总览**（三线程分离 + D3D11 零拷贝 + 独立 UI 窗口）：
```
读包线程(demuxLoop)
  ├─ 音频包 → 音频包队列 → 音频解码线程(audioDecodeLoop) → 音频帧队列 → SDL回调播放(软件音量缩放)
  │     └─ AudioTap(16kHz mono float32) → Whisper ASR → HY-MT2 翻译 → AI字幕(双轨)
  └─ 视频包 → 视频包队列 → 视频解码线程(videoDecodeLoop)
                → D3D11VA硬解/软解回退 → GPU内拷贝到中转纹理池 → 视频帧队列(纹理+PTS)
                → 主线程QTimer(1ms) PTS同步 → D3D11渲染(QWindow+交换链, Present(0,0))

UI层：主窗口(纯视频) + 独立uiWindow(设置栏+控制栏+播放列表面板)，渐入渐出
字幕层：独立可拖动悬浮窗(DraggableLabel)，字幕开关与文件选择分离
```

**核心策略**：可配置质量（硬件解码开关 + 分辨率选项），**纯手动切换**（不做自动降画质，避免 bug）。

---

## 二、开发大纲（ROADMAP 状态）

大纲文件：`D:\new\Player\ROADMAP_v4.md`（v1/v2/v3 为历史版本）

| 里程碑 | 主题 | 状态 |
|--------|------|------|
| M1 | 基础播放框架（三线程+软解+SDL音频+Qt渲染） | ✅ 完成 |
| M2 | 硬件解码 + D3D11 零拷贝渲染（无损视频核心） | ✅ 完成 |
| M3 | 交互完善（seek/音量/全屏/配置持久化/快捷键/UI架构重构） | ✅ 完成 |
| M4 | 高级功能（T4.1-T4.9） | 🔄 T4.1-T4.7 ✅，T4.8 进行中，T4.9 ✅ |
| M5 | 优化完善（稳定性/性能/UI美化/格式兼容，可选） | ⏳ 未开始 |

**T4.8 四阶段计划**（见 `T4.8_AI_SUBTITLE_PLAN.md`）：
- Phase 1：离线完整识别+翻译（验证效果）— 大部分完成，质量不达标
- Phase 2：VAD 分段准实时（延迟 5-10 秒）— 未开始
- Phase 3：接近实时优化（延迟 3-5 秒）— 未开始
- Phase 4：产品化与传播 — 未开始

---

## 三、环境与依赖清单（在哪里）

### 3.1 开发工具链

| 组件 | 版本/路径 | 说明 |
|------|-----------|------|
| Qt | `C:/Qt/6.12.0/msvc2022_64/` | 6.12.0，MSVC 2022 64 位 |
| 编译器 | Visual Studio 2026 (MSVC) | CMake Generator: `Visual Studio 18 2026` |
| CMake | 系统安装 | 配置命令见 6.1 |
| 编译线程 | **必须单线程 `-j 1`** | 多线程会 C1060 堆空间不足（MSVC /Zm2000 已设置） |

### 3.2 第三方库（`D:\new\Player\third_party\`）

| 库 | 路径 | 版本/说明 |
|----|------|-----------|
| FFmpeg | `third_party\ffmpeg\` | **BtbN LGPL shared**（已从 GPL 替换为 LGPL，法律合规） |
| SDL2 | `third_party\SDL2\` | 2.32.4 VC 版本，`#include <SDL.h>`，需 `SDL_MAIN_HANDLED` |
| whisper.cpp | `third_party\whisper.cpp\` | 1.9.4-dev，CMake add_subdirectory |
| llama.cpp | `third_party\llama.cpp\` | commit 4ebdf2c，0.5.0-dev，CMake add_subdirectory |
| llama-cli 测试工具 | `third_party\llama.cpp\build_test\bin\Release\llama-cli.exe` | 用于独立验证翻译模型 |
| ffmpeg.exe | `third_party\ffmpeg\bin\ffmpeg.exe` | 视频剪辑工具 |

### 3.3 AI 模型文件（`D:\new\Player\third_party\models\` 与 whisper models）

| 模型 | 路径 | 大小 | 用途 |
|------|------|------|------|
| HY-MT2-1.8B-Q4_K_M.gguf | `third_party\models\Hy-MT2-1.8B-Q4_K_M.gguf` | 1.06GB | 翻译模型（hunyuan-dense 架构，llama.cpp 原生支持） |
| ggml-small.bin | `third_party\whisper.cpp\models\ggml-small.bin` | 465MB | 当前使用的 ASR 模型（small 档） |
| ggml-tiny.bin | `third_party\whisper.cpp\models\ggml-tiny.bin` | 74MB | 备用轻量模型 |

### 3.4 数据 / 日志 / 产物目录

| 目录 | 路径 | 内容 |
|------|------|------|
| 截图 | `D:\new\Player\picture\` | T4.6 截图保存目录 |
| 运行日志 | `D:\new\Player\logs\` | Logger 自动写入 `mediaplayer_时间戳.log` |
| 崩溃报告 | `D:\new\Player\crash_reports\` | CrashHandler 生成 `.dmp` + `.txt` |
| 诊断日志 | `D:\new\Player\diag.log` | Debug 模式 Tee-Object 输出（**注意：根目录有个 `diag.log、` 是误建的带顿号文件，可忽略/删除**） |
| 桌面 | `C:\Users\carry\OneDrive\Desktop\` | Ctrl+E 导出诊断包 zip、剪辑输出视频都在此 |

### 3.5 关键源文件索引（`D:\new\Player\`）

| 文件 | 职责 |
|------|------|
| `main.cpp` | 主窗口+独立uiWindow、全屏、快捷键、设置持久化、字幕悬浮窗、播放列表、截图、拖拽、AI识别按钮+目标语言下拉框、T4.9集成 |
| `PlayerController.h/cpp` | 核心控制器：三线程解码、D3D11VA+软解回退、音频时钟、PTS同步、seek（含防抖）、软件音量、播放列表、AudioTap、aiSubtitles()、diagnosticStats() |
| `VideoWidget.h/cpp` | 视频窗口（QWindow + GDI 黑色背景 + 截图接口） |
| `D3D11Renderer.h/cpp` | 交换链/着色器/中转纹理池(StagingTexturePool)/全局锁(g_d3d11Mutex)/截图回读 |
| `SubtitleParser.h/cpp` | SRT/ASS/SSA 解析 + setCues() + 双轨字幕（translatedText） |
| `WhisperTranscriber.h/cpp` | whisper.cpp 封装（loadModel/transcribe/transcribeAsync/cancel） |
| `Translator.h/cpp` | llama.cpp 封装（loadModel/translate/translateBatchAsync/buildPrompt/infer + 后处理 + 多语言过滤 + 任务ID） |
| `Logger.h/cpp` | T4.9 结构化日志（LVL_ 前缀枚举、环形缓冲500行、文件轮转） |
| `CrashHandler.h/cpp` | T4.9 SEH 捕获 + minidump + 调用栈（延迟1秒安装） |
| `SystemInfo.h/cpp` | T4.9 系统信息采集 |
| `DiagnosticPanel.h/cpp` | T4.9 Ctrl+D 实时诊断面板（500ms刷新，asprintf 格式化） |
| `DiagnosticExporter.h/cpp` | T4.9 Ctrl+E 导出 zip（SHGetKnownFolderPath 真实桌面路径） |
| `CMakeLists.txt` | 构建配置（/Zm2000、WIN32_EXECUTABLE仅Release、POST_BUILD拷贝DLL） |

---

## 四、已完成进度总结

### ✅ 已完成并通过验收

- **M1 基础播放**：三线程架构、FFmpeg 解码（D3D11VA+软解回退）、SDL2 无损音频直通、播放/暂停、进度条、背压机制
- **M2 D3D11 零拷贝**：QWindow+原生交换链+NV12→RGB着色器、中转纹理池、D3D11VA lock回调、设备全局复用、4K 60fps 稳定
- **M3 交互**：seek、音量、全屏、设置持久化、快捷键、独立UI窗口方案（解决叠加/重影）、全屏无白色矩形、切换视频稳定
- **T4.1** 软解回退（sws_scale→NV12→动态纹理→中转纹理）
- **T4.2** 发布打包（Release 无控制台）
- **T4.3** 高帧率（1ms PreciseTimer + Present(0,0)）
- **T4.4** 字幕（SRT/ASS/SSA + 可拖动悬浮窗 + 开关与文件选择分离）
- **T4.5** 播放列表（添加/移除/双击/自动连播）
- **T4.6** 截图（D3D11 回读 → picture 目录 + 淡入淡出通知）
- **T4.7** 拖拽打开（三处安装过滤器）
- **T4.9** 高效反馈系统（Logger + CrashHandler + Ctrl+D 诊断面板 + Ctrl+E 导出诊断包）— 已通过 5 次连续运行稳定性测试

### 🔄 部分完成（T4.8 离线 AI 翻译字幕）

已完成：
- ✅ whisper.cpp 集成（MSVC 编译 /Zm2000 单线程修复）
- ✅ AudioTap（16kHz mono float32 录制）
- ✅ WhisperTranscriber 类 + AI 识别按钮
- ✅ HY-MT2-1.8B 模型验证（llama-cli 实测"Hello world"→"你好世界"）
- ✅ Translator 类 + llama.cpp C API 封装 + 批量翻译 + 跨线程 UI 安全
- ✅ 双轨字幕显示（原文/译文切换）
- ✅ 目标语言下拉框 + 切换语言自动重新翻译
- ✅ 翻译后处理链：非语音标记过滤、思考过程过滤、原文去重、多语言字符过滤、任务ID防交错

未完成/不达标：
- ❌ **翻译质量**（用户验收不通过：准确率低、多语言混合、思考过程残留）
- ❌ **seek 卡顿/音画不同步**（已做修复待验收：80ms 防抖 + 音频早帧丢弃 + 视频预缓冲）
- ⬜ 识别结果缓存为 SRT（Phase 1 剩余）
- ⬜ UI 目标语言选择持久化（Phase 1 剩余）
- ⬜ Phase 2-4（VAD 分段准实时、接近实时、产品化）全部未开始

### ✅ 法律合规（T4.8 前置）

- FFmpeg GPL → **LGPL**（BtbN LGPL shared）
- Silero VAD 确认 MIT
- LICENSE（MIT）+ licenses/ 7 个组件许可证 + 关于对话框开源组件列表 + EULA 免责声明

---

## 五、当前存在的问题（未解决清单）

### 5.1 严重问题

1. **翻译准确率低且多语言混合**（T4.8 核心未决）
   - 症状：选择韩语/英语时仍夹杂中文翻译；部分字幕是 AI 思考过程而非译文；准确率不理想
   - 已做修复：prompt 格式、后处理过滤、多语言字符集过滤、任务ID 防交错、KV cache 清空、解码参数调优（temp 0.15 / rep 1.4 / max_tokens 48）
   - 未验证：最近一轮修复尚未经用户实测
   - 可能根因：HY-MT2-1.8B 轻量模型质量上限 + whisper small 识别误差传播 + prompt 仍有优化空间

2. **seek 卡顿与音画不同步**（T4.8 关联）
   - 症状：快捷键快进快退直接卡住、拖动进度条卡顿、音画不同步
   - 已做修复：80ms seek 防抖、音频解码丢弃早于目标位置的帧、startPlaybackThreads 增加视频/音频预缓冲（少量）
   - 未验证：最近一轮修复尚未经用户实测

3. **随机闪退 / 崩溃**（T4.9 捕获到多次）
   - 最新崩溃：`crash_reports\crash_20261003_144842.txt` — ACCESS_VIOLATION (0xC0000005)，RAX=0xdddddddddddddddd（MSVC 释放后填充值），典型 **use-after-free（访问已释放内存）**
   - 发生场景：T4.8 翻译集成阶段，崩溃日志只有启动记录（崩溃发生较早）
   - 排查方向：翻译/识别线程与 UI 线程的生命周期竞争、线程停止时对象销毁顺序、llama/whisper 上下文释放
   - 注意：12:48-14:48 期间有 12 份崩溃报告，说明该时段问题密集

### 5.2 已知限制（非 bug）

- 个别关键帧间隔过长的视频，seek 到末尾几秒恢复慢（视频特性，用户曾接受"不再优化"，但最新反馈快进快退卡住更严重，需重新评估）
- 软解回退路径未充分端到端验证
- 仅 Windows 平台（D3D11 绑定）
- 字幕样式暂不支持自定义
- 不支持倍速/音轨切换/HDR（M5 可选）

### 5.3 未决决策

- 是否升级翻译模型（HY-MT2-1.8B Q8 / 更大模型）提升质量，权衡体积与速度
- whisper 是否保持 small（当前 465MB）还是再升 medium
- Release 部署形态（当前 Debug 有控制台）
- 项目是否开源（LGPL FFmpeg 后可选闭源/开源）
- 产品独特命名（当前 "MediaPlayer" 太通用，法律风险清单未执行完）
- 宣传素材不使用版权视频画面（未执行完）

---

## 六、新对话无缝衔接操作手册

### 6.1 编译

```powershell
cd D:\new\Player
# 首次或清理 build 后：
cmake -B build -G "Visual Studio 18 2026" -A x64 -DQt6_DIR="C:/Qt/6.12.0/msvc2022_64/lib/cmake/Qt6"
# 编译（必须单线程！）：
cmake --build build --config Debug -j 1
```

### 6.2 运行 / 诊断

```powershell
# 直接运行（Debug 有控制台）：
& "D:\new\Player\build\Debug\MediaPlayer.exe"
# 诊断模式（捕获全部日志到 diag.log）：
& "D:\new\Player\build\Debug\MediaPlayer.exe" 2>&1 | Tee-Object D:\new\Player\diag.log
# Ctrl+D 实时诊断面板 / Ctrl+E 导出诊断包到桌面
```

### 6.3 用户反馈日志时优先索要

1. `D:\new\Player\diag.log`（诊断模式运行后生成，含 [Translator] Raw/Final 关键行）
2. `D:\new\Player\logs\` 最新日志文件
3. `D:\new\Player\crash_reports\` 最新崩溃报告（.txt 即可，必要时 .dmp）

### 6.4 独立验证翻译模型

```powershell
& "D:\new\Player\third_party\llama.cpp\build_test\bin\Release\llama-cli.exe" -m "D:\new\Player\third_party\models\Hy-MT2-1.8B-Q4_K_M.gguf" -p "中文：Hello world" -n 64
```

### 6.5 视频剪辑

```powershell
& "D:\new\Player\third_party\ffmpeg\bin\ffmpeg.exe" -ss 00:04:50 -i "输入.mp4" -t 30 -c copy "输出.mp4" -y
```

### 6.6 文档管理规则（用户强制要求）

- **MD 文件每次更新必须另存为新文件**，禁止覆盖旧版本（PROGRESS.md 除外，直接更新覆盖）
- 开发总结/方案文件：`D:\new\Player\` 下按里程碑命名（M1~M4_DEVELOPMENT_SUMMARY.md）
- 新对话开始：先读 `PROJECT_HANDOVER.md` + `PROGRESS.md` + `ROADMAP_v4.md` 恢复上下文

---

## 七、下一步建议（按优先级）

1. **验证最近一轮修复**（翻译质量 + seek 卡顿）：让用户实测，收集 diag.log 中 [Translator] Raw/Final 判断翻译问题根因
2. **修复崩溃**：分析 crash_reports 中 ACCESS_VIOLATION（0xdddddddddddddddd = use-after-free），重点排查翻译/识别线程生命周期与对象销毁顺序
3. 完成 T4.8 Phase 1 剩余（SRT 缓存、语言选择持久化）
4. 用户确认翻译质量达标后，进入 Phase 2（VAD 分段准实时）
5. 质量不达标则评估模型升级（HY-MT2 Q8 或替换方案），符合用户"日/英/韩/兼容性/体积<10GB"四项约束

---

*本文档由 PROJECT_HANDOVER.md 承载，供任何新对话直接读取恢复上下文。*
