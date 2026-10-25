# PROGRESS

> 本文件是唯一可原地覆盖的进度文档。最后更新：2026-10-25（v0.4.7）

## 版本线

| tag | 内容 |
|---|---|
| `v0.4.0` | M1–M4 基线入库（可直接编译运行的原型） |
| `v0.4.1` | 崩溃可诊断 + 三处生命周期/同步缺陷（见下） |
| `v0.4.2` | 翻译链路改用模型自带对话模板 + 无 GUI 回归工具 |
| `v0.4.3` | 删除死代码，抽取队列/线程辅助函数（净减约 108 行） |
| `v0.4.4` | 文档收敛（AGENTS / README）+ 版本号统一到 `Version.h` |
| `v0.4.5` | ROADMAP v5：产品定位 + 18 条可验收需求 + M5-M9 路线 |
| `v0.4.6` | M5 起步：模型路径配置化（AppPaths）、模型异步加载可 join、手动验收清单 |
| `v0.4.7` | 纯函数单测（26 断言）+ ctest 入口 + 最小 CI 工作流（未实跑） |

## 本轮修掉的问题

1. **随机崩溃 0xC0000005（RAX=0xdddddd…）** — D3D11 自建设备交给 FFmpeg 的 `AVHWDeviceContext` 时没有 `AddRef`，而 `d3d11va_device_uninit` 会无条件 `Release`，导致设备提前销毁、析构时二次释放。已补 `AddRef`（device 与 device_context 各一次）。
2. **stop/seek 偶发永久卡死** — 置 `m_isRunning=false` 与 `wakeAll()` 不在同一把锁内，worker 丢唤醒后进入无超时 `wait()`。已抽 `signalWorkersToStop()`，全部在对应 mutex 内置位再唤醒。
3. **seek 卡顿约 500ms** — `startPlaybackThreads` 曾在 GUI 线程 `msleep(10)` 轮询预缓冲（音频 5 帧 + 视频 1 帧）。已移除轮询；音频回调在队列空时输出静音，首帧到达后自然衔接。
4. **seek 后音画错位** — 全工程没有 `SDL_ClearQueuedAudio`，SDL 设备队列里残留的 PCM 会继续播。已在 `stopPlaybackThreads()` 中暂停并清空设备队列。
5. **翻译输出另一种语言 / 混入思考过程** — 根因是 `buildPrompt` 只拼 `原文\n语言：`，模型当普通文本续写；且语言过滤在没有目标字符时回退到原文。已改为按 gguf 内 chat 模板构造对话、`parse_special=true` 分词、温度放到采样链末尾、只取首个有效行后处理、同语言直接透传。
6. **崩溃报告不可用** — 原来只 `MiniDumpWriteDump`，`.txt` 里是裸地址。已加 DbgHelp 符号化（`模块!函数+偏移 (文件:行)`，40 帧，失败回退裸地址）。
7. **未定义行为** — `WhisperTranscriber`/`Translator` 析构不等工作线程（use-after-free）、`main.cpp` 用 detached `std::thread` 加载模型、AI 识别按钮外层多包了一层 `QThread`。已全部改为可 cancel + join。

## 当前状态

- 构建：`cmake --build build --config Debug -j 8` 通过，无新增告警（既有 `MSB8027` 来自 llama.cpp 同名源文件，可忽略）。
- 翻译回归：`translate_cli` + `tests/translate_samples.txt`，中文/英语/日语/韩语四向各 7/7 非空，单句 1.1–1.8 秒，无空译、无第二语言行。
- 需求达成（对照 ROADMAP v5）：M1–M3 完成；M4 除"AI 字幕端到端体验"外完成；M5 进行中（D-02 模型路径配置化、ACCEPTANCE 清单已完成，长跑/软解/单测/CI 未开始）。
- 模型定位：启动日志会打印 `resource ai/whisperModel found at: ...`；配置项 `ai/whisperModel`/`ai/translatorModel` 可覆盖，便携包把模型放 `models/` 即可被优先命中。

## 已知问题 / 未验证

1. seek 的**端到端**效果需要在 GUI 里手动回归（本轮只做了代码级修复 + 编译验证，没有跑视频）。
2. 软解回退路径仍未充分端到端验证（硬解模式正常）。
3. 翻译对"中文目标 + 日语源"的简体/繁体倾向依赖模型自身，未做后处理归一。
4. `main.cpp` 965 行、`PlayerController.cpp` 1251 行仍是单文件单类，拆分方案见 `docs/REFACTOR_PLAN.md`（未执行）。
5. 自动化测试只覆盖纯函数（26 断言 / 0.13s）；播放链路仍靠 `docs/ACCEPTANCE.md` 手动回归。CI 工作流已写但**未在 Actions 上跑过一次**。
6. Silero VAD 只出现在许可清单与计划文档里，**未实现**；不要对外宣称有 VAD。

## 下一步（按 ROADMAP v5）

1. **M5 质量基线**（已完成：D-02 模型路径配置化、ACCEPTANCE 清单、纯函数单测、CI 骨架；未完成：2 小时长跑 + 切换 50 次、软解回退端到端、CI 实跑）。
2. **M6 实时字幕**：音频环形缓冲 → Silero VAD（当前未实现）→ 分段 ASR → 带上下文的翻译 → 端到端延迟打点，目标 ≤ 8s。
3. 拆文件仍登记为技术债 D-01，按需抽取，不作为独立任务（触发条件见 `docs/REFACTOR_PLAN.md`）。
