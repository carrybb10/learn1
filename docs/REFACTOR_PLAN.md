# 拆文件方案（未执行）

现状：`main.cpp` 965 行只有一个 `main()`，里面 57 个 lambda；`PlayerController.cpp` 1251 行一个类，`videoDecodeLoop` 单函数 140 行。
能跑，但读一次要滚很久，改动风险集中。下面是按"改动小、收益大"排序的拆法，每步都能单独编译验证。

## 原则

- **先搬代码，不改逻辑**。每步只做"剪切 → 新文件 → 加头文件 → 编译通过 → 提交"。
- 不做大爆炸重写：不做依赖注入框架，不引入新库。
- 拆分以"有没有独立的成员/状态"为准；只是代码长不算，那应该抽函数。

## PlayerController 拆（优先）

按状态边界拆，而不是按行数：

| 目标文件 | 搬什么 | 需要的成员 |
|---|---|---|
| `AudioOutput.h/cpp` | `initAudio`、`audioCallback`、`sdlAudioCallback`、`setVolume`、`avToSdlFormat` | `m_audioDevice`、输出格式字段、`m_volume`、`m_audioPlayedSamples` |
| `FramePresenter.h/cpp` | `displayFrame`、时钟插值、`positionChanged` 发射 | 视频帧队列、`m_seekTime`、时钟字段 |
| `TextureUploader.h/cpp` | `ensureSwUploadTexture`、`uploadSoftwareFrame` | `m_swUploadTexture`、`m_swsCtx` |
| `AudioTap.h/cpp` | `startAudioTap`/`stopAudioTap`/`getAudioTapSamples` | tap 缓冲、swr、mutex |

剩下的 `PlayerController` 保留线程编排（demux/decode/seek/transport）——这部分本来就该由它管。

**顺序**：`AudioTap`（最独立）→ `TextureUploader` → `FramePresenter` → `AudioOutput`（要动音频时钟的归属，最后做）。

## main.cpp 拆

`main()` 里按"哪块 UI"切成自由函数，状态显式传参，避免再靠 `[&]` 捕获一堆局部变量：

1. `ui/SettingsBar.*` — 设置栏（硬件解码下拉、音量、字幕开关、AI 按钮）
2. `ui/ControlBar.*` — 播放/暂停/进度条/时间显示/全屏
3. `ui/PlaylistPanel.*` — 播放列表面板
4. `ui/SubtitleOverlay.*` — 可拖动字幕窗
5. `ui/Shortcuts.*` — 键盘绑定
6. `AppContext` 结构体 — 把 `player`/`translator`/`whisper`/`settings` 收成一个对象，各 UI 模块拿引用

**先做 6**（纯收拢，不动逻辑），再做 1–5。每拆一个模块，`main()` 就少一段。

## 验收

- 每个文件 < 400 行；`main()` < 80 行。
- 拆完仍能 `cmake --build` 通过，`translate_cli` 回归 7/7。
- 手动回归：播放/暂停/seek/全屏/切视频/拖拽打开。
