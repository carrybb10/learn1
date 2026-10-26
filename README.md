# MediaPlayer

一个从零手写的 Windows 桌面视频播放器：**Qt 6 界面 + FFmpeg 解码 + D3D11 零拷贝渲染 + SDL2 音频**，
并在本地离线跑 **Whisper 语音识别 + 1.8B 翻译模型** 生成双语字幕。

> 目标不是"又一个播放器"，而是完整走一遍**高帧率渲染、音视频同步、GPU 资源生命周期、离线推理集成**
> 这些真正会出事的工程问题。

## 亮点

| | |
|---|---|
| **D3D11 零拷贝渲染** | 解码纹理在中转纹理池内 GPU 间拷贝，NV12→RGB BT.709 着色器直出交换链；4K 60fps 实测 decodeFps=60 / displayFps=60 |
| **三线程解码 + 音频时钟同步** | 读包 / 音频解码 / 视频解码分离，背压队列；主线程 1ms 精确定时器按 PTS 出帧，音频时钟由 SDL 已播样本数插值 |
| **离线 AI 字幕** | whisper.cpp 流式识别 + Hy-MT2-1.8B（GGUF / llama.cpp）翻译，中英日韩四向，全程无网络；用回归 CLI + 固定语料保证翻译质量不退化 |
| **可诊断** | 结构化日志 + `MiniDumpWriteDump` 崩溃转储 + **DbgHelp 符号化调用栈** + 实时诊断面板（Ctrl+D）+ 一键诊断包（Ctrl+E） |

## 架构

```
demuxLoop ──► 音频包队列 ──► audioDecodeLoop ──► 音频帧队列 ──► SDL 回调（软件音量）
          └─► 视频包队列 ──► videoDecodeLoop ──► D3D11VA 硬解 / 软解回退（sws → NV12 → 动态纹理）
                                              └─► 中转纹理池 ──► 视频帧队列(纹理+PTS)
                                                    └─► 主线程 QTimer(1ms) PTS 同步 ──► 交换链 Present(0,0)
```

- 主窗口只承载视频（`QWindow` + 原生交换链），控制栏/播放列表放在**独立无边框窗口**，避开 Qt 子控件与 `createWindowContainer` 的混合渲染问题。
- 字幕是独立可拖动悬浮窗；AI 识别结果按句注入字幕轨，翻译结果跟随语言切换。

## 构建

需要：Windows 10/11、Visual Studio 2022/2026（MSVC）、CMake ≥ 3.20、Qt 6.12（msvc2022_64）。

```powershell
git clone <this-repo> Player && cd Player
powershell -ExecutionPolicy Bypass -File .\scripts\fetch-deps.ps1   # 拉取 FFmpeg / SDL2 / llama.cpp / whisper.cpp 与模型
cmake -B build -G "Visual Studio 18 2026" -A x64 -DQt6_DIR="C:/Qt/6.12.0/msvc2022_64/lib/cmake/Qt6"
cmake --build build --config Release -j 8
C:\Qt\6.12.0\msvc2022_64\bin\windeployqt.exe build\Release\MediaPlayer.exe
.\build\Release\MediaPlayer.exe
```

模型不入仓库：把 `Hy-MT2-1.8B-Q4_K_M.gguf` 放进 `third_party/models/`，或把模型放在 exe 旁的 `models/` 目录（便携形态），
也可以在 exe 旁放 `MediaPlayer.ini`（模板见 `scripts/MediaPlayer.ini.example`）用 `[ai]` 段指定绝对路径，
注册表 `HKCU\Software\MediaPlayer\Config` 同样可覆盖。启动日志会打印最终命中的路径。

打包便携版（Qt 运行时 + 第三方 DLL，模型需自备）：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\package.ps1
# -> dist\ 与 MediaPlayer-v<版本>-win64.zip
```

单元测试（不需要模型和显卡）：

```powershell
cmake --build build --config Debug --target unit_tests -j 8
ctest --test-dir build -C Debug --output-on-failure
```

依赖版本固化在 [`docs/THIRD_PARTY_VERSIONS.md`](docs/THIRD_PARTY_VERSIONS.md)。CI 状态：
[![build](https://github.com/carrybb10/learn1/actions/workflows/build.yml/badge.svg)](https://github.com/carrybb10/learn1/actions/workflows/build.yml)

## 操作

| 键 | 功能 |
|---|---|
| `Space` | 播放 / 暂停 |
| `←` `→` | 后退 / 前进 5 秒 |
| `↑` `↓` | 音量 ±5 |
| `F` / `Esc` | 全屏切换 / 退出全屏 |
| `Ctrl+D` | 诊断面板 |
| `Ctrl+E` | 导出诊断包 |

## 目录

```
main.cpp              窗口、控制栏、快捷键、设置持久化、字幕悬浮窗
PlayerController.*    解码线程、音画同步、seek、音量、播放列表、AudioTap
D3D11Renderer.*       交换链、着色器、中转纹理池、截图回读
VideoWidget.*         纯视频 QWindow
SubtitleParser.*      SRT/ASS 解析
Translator.*          Hy-MT2 (llama.cpp) 翻译：chat 模板、采样、后处理
WhisperTranscriber.*  whisper.cpp 封装（可取消、可等待）
tools/translate_cli   无 GUI 翻译回归入口
tests/                固定语料
logs/ crash_reports/  运行期产物（不入库）
```

## 状态

已实现：M1 基础播放、M2 D3D11 零拷贝、M3 交互（seek/音量/全屏/设置持久化）、M4 字幕/播放列表/截图/离线 AI 字幕/诊断系统。
时间线见 [`ROADMAP.md`](ROADMAP.md)，当前进度与已知问题见 [`PROGRESS.md`](PROGRESS.md)。

## 许可

本项目代码 MIT。第三方组件许可见 [`licenses/`](licenses/)：FFmpeg（LGPL v2.1，BtbN shared build）、Qt（LGPL v3）、SDL2（zlib）、whisper.cpp / llama.cpp（MIT）。
模型权重不随仓库分发，需自行下载（见 `scripts/fetch-deps.ps1`）。
