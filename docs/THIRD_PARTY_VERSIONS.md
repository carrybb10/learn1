# 依赖版本清单（可复现性锚点）

记录 2026-10-25 基线 v0.4.0 构建时的精确版本。`third_party/` 不入库，靠本文件 + `scripts/fetch-deps.ps1` 重建。

## 工具链

| 组件 | 版本 / 路径 |
|---|---|
| Qt | 6.12.0 · `C:\Qt\6.12.0\msvc2022_64` |
| 编译器 | MSVC · Visual Studio 18 2026 |
| CMake | 3.20+ · 生成器 `Visual Studio 18 2026` |
| Git | 2.54.0.windows.1 |

## 第三方源码（`third_party/`）

| 库 | 版本锚点 |
|---|---|
| llama.cpp | commit `4ebdf2c74acce30883d8e34b7c70b3eb8146f2fe` |
| whisper.cpp | commit `60c0be6ac8fa71b1a2ae2dd938a31a34a508e774` |
| FFmpeg | BtbN **LGPL shared** 构建（`avcodec-63.dll` / `avformat-63.dll` / `avutil-61.dll`）——**不得换回 GPL 版** |
| SDL2 | 2.32.x（`third_party/SDL2/include/lib` 预编译） |

## 模型（`third_party/` 下，不入库）

| 模型 | 大小 | SHA256 前 16 位 | 用途 |
|---|---|---|---|
| `models/Hy-MT2-1.8B-Q4_K_M.gguf` | 1080.6 MB | `DC5F44FCF1FA496E` | 离线翻译 |
| `whisper.cpp/models/ggml-small.bin` | 465.0 MB | `1BE3A9B2063867B9` | 语音识别（默认） |
| `whisper.cpp/models/ggml-tiny.bin` | 74.1 MB | `BE07E048E1E599AD` | 低配回退 |

## 基线产物校验

`build/Debug/MediaPlayer.exe` SHA256 前 16 位：`340534FC4E49664E`（2026-10-25 17:17 构建）。

## 已知未使用依赖

`licenses/Silero_VAD_MIT.txt` 已列入许可清单，但工程内**没有 VAD 实现**（`third_party/whisper.cpp/models/for-tests-silero-v6.2.0-ggml.bin` 仅为上游测试模型）。二选一：实现 VAD，或在下个版本移除该许可声明。
