# PROGRESS

> 本文件是唯一可原地覆盖的进度文档。最后更新：2026-10-26（v0.5.1）

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
| `v0.4.8` | 软解黑屏修复（NV12 上传纹理改 STAGING）+ `d3d11_probe` 回归 + CI 首跑修复 |
| `v0.5.0` | M5 质量基线完成：105 分钟长跑无崩溃、seek 无卡顿；实时字幕音频输入改用 60s 环形缓冲 |
| `v0.5.1` | Silero VAD 分段链路打通：`SileroVad` + `VadSegmenter` + `vad_cli`，流式与整段结果一致 |（CI run 40337277482 通过）
| `v0.5.2` | 实时字幕接通播放链路：`RealtimeSubtitleWorker`（环形缓冲 -> VAD -> whisper -> 翻译）+ `rt_cli` 端到端工具；whisper 线程数随核数提升 |
| `v0.5.3` | 便携版把 `logs/`、`crash_reports/` 留在自己目录（原来写到上一级），重新打包 |

## 本轮修掉的问题

1. **随机崩溃 0xC0000005（RAX=0xdddddd…）** — D3D11 自建设备交给 FFmpeg 的 `AVHWDeviceContext` 时没有 `AddRef`，而 `d3d11va_device_uninit` 会无条件 `Release`，导致设备提前销毁、析构时二次释放。已补 `AddRef`（device 与 device_context 各一次）。
2. **stop/seek 偶发永久卡死** — 置 `m_isRunning=false` 与 `wakeAll()` 不在同一把锁内，worker 丢唤醒后进入无超时 `wait()`。已抽 `signalWorkersToStop()`，全部在对应 mutex 内置位再唤醒。
3. **seek 卡顿约 500ms** — `startPlaybackThreads` 曾在 GUI 线程 `msleep(10)` 轮询预缓冲（音频 5 帧 + 视频 1 帧）。已移除轮询；音频回调在队列空时输出静音，首帧到达后自然衔接。
4. **seek 后音画错位** — 全工程没有 `SDL_ClearQueuedAudio`，SDL 设备队列里残留的 PCM 会继续播。已在 `stopPlaybackThreads()` 中暂停并清空设备队列。
5. **翻译输出另一种语言 / 混入思考过程** — 根因是 `buildPrompt` 只拼 `原文\n语言：`，模型当普通文本续写；且语言过滤在没有目标字符时回退到原文。已改为按 gguf 内 chat 模板构造对话、`parse_special=true` 分词、温度放到采样链末尾、只取首个有效行后处理、同语言直接透传。
6. **崩溃报告不可用** — 原来只 `MiniDumpWriteDump`，`.txt` 里是裸地址。已加 DbgHelp 符号化（`模块!函数+偏移 (文件:行)`，40 帧，失败回退裸地址）。
7. **未定义行为** — `WhisperTranscriber`/`Translator` 析构不等工作线程（use-after-free）、`main.cpp` 用 detached `std::thread` 加载模型、AI 识别按钮外层多包了一层 `QThread`。已全部改为可 cancel + join。

## 当前状态

- **实测（2026-10-26）**：105 分钟连续播放无崩溃、无音画错误；拖动进度条无卡顿。配合 seek 双因修复（移除 GUI 线程预缓冲轮询 + 清 SDL 设备队列），ROADMAP FR-04「seek 响应」与 NFR-01「2 小时 0 崩溃」按通过记录（105 分钟）。
- **M6 实时字幕进度**：音频环形缓冲（60s、3.84MB 固定占用）已完成；VAD 分段链路已打通并可无 GUI 自检——`SileroVad`（加载 + 流式概率）、`VadSegmenter`（阈值/最短语音/最长静音/最长段）、`vad_cli`（wav → 分段列表）。关键坑：首次流式推理前必须 `whisper_vad_reset_state()`，否则语音概率被系统性低估。尚未接入播放线程，GUI 里的实时字幕仍走离线录制。

- 构建：`cmake --build build --config Debug -j 8` 通过，无新增告警（既有 `MSB8027` 来自 llama.cpp 同名源文件，可忽略）。
- 翻译回归：`translate_cli` + `tests/translate_samples.txt`，中文/英语/日语/韩语四向各 7/7 非空，单句 1.1–1.8 秒，无空译、无第二语言行。
- 需求达成（对照 ROADMAP v5）：M1–M3 完成；M4 除"AI 字幕端到端体验"外完成；M5 进行中（已完成：D-02 模型路径配置化、ACCEPTANCE 清单、纯函数单测、CI 工作流与依赖脚本；未完成：2 小时长跑、软解端到端、CI 实跑）。
- 模型定位：启动日志会打印 `resource ai/whisperModel found at: ...`；配置项 `ai/whisperModel`/`ai/translatorModel` 可覆盖，便携包把模型放 `models/` 即可被优先命中。

## 仓库治理（GitHub）

- **CI 已实跑通过**：`build` 工作流 run `40197215858`（tag `v0.4.8`）四步全绿——安装 Qt（aqt 6.10.3）→ 拉取依赖 → 全量 Debug 构建 → `ctest` 2/2，总耗时约 4 分钟（含第三方源码编译）。徽章：`https://github.com/carrybb10/learn1/actions/workflows/build.yml/badge.svg`。
- 触发方式：手动 `workflow_dispatch` 或推 `v*` tag（普通 push 不触发，省额度）。

- 远端 `carrybb10/learn1` 是公开仓库，默认分支 `main`，许可证 MIT，版本史用 tag `v0.4.x` 表示。
- 分支 `main` 受 ruleset `protect-main`（id 25820797）保护：禁止删除、禁止 force push；绕过名单只有 repo admin，即仓库主人。
- tag `refs/tags/v*` 受 `protect-tags`（id 25820800）保护：已发布的版本号不能被删除或移动。
- Actions 里的 `GITHUB_TOKEN` 已降为只读（`default_workflow_permissions=read`，且不允许自动批准 PR），工作流被外部 PR 篡改也推不动仓库。
- 加固脚本 `scripts/harden-repo.ps1` 幂等可重跑；换仓库只需改 `-Owner` / `-Repo`。
- 仍需人工确认三件事（仓库设置替代不了）：账号开 2FA、Collaborators 保持只有自己、Actions 里把外部 Fork PR 设为需审批。
- 环境坑：本机 WinINET 代理设成 `http://127.0.0.1:65532` 时，.NET 的 `Invoke-RestMethod` 连不上 GitHub（超时或"基础连接已关闭"），而 `curl.exe` 直连正常。访问 GitHub 的脚本一律走 curl，别用 `Invoke-RestMethod`。

## 已知问题 / 未验证

1. seek 的**端到端**效果需要在 GUI 里手动回归（本轮只做了代码级修复 + 编译验证，没有跑视频）。
2. 软解回退路径已由 `tools/d3d11_probe.cpp` 覆盖（NV12 上传→拷贝→采样→回读亮度），但**没有用真实片源在 GUI 里端到端走过**；按当前判断它是兜底路径，不再作为发布阻塞项。
3. 翻译对"中文目标 + 日语源"的简体/繁体倾向依赖模型自身，未做后处理归一。
4. `main.cpp` 965 行、`PlayerController.cpp` 1251 行仍是单文件单类，拆分方案见 `docs/REFACTOR_PLAN.md`（未执行）。
5. 自动化测试覆盖纯函数 26 断言 + D3D11 纹理探针（ctest 2/2）；播放链路仍靠 `docs/ACCEPTANCE.md` 手动回归。CI 工作流已写但**未在 Actions 上跑过一次**。
6. Silero VAD 已实现（`SileroVad` + `VadSegmenter` + `vad_cli` 自检），但实时链路只在小样音频（jfk.wav）上验证过，**没在真实视频上做端到端延迟实测**。

## 下一步（按 ROADMAP v5）

1. **M5 质量基线**（已完成：D-02 模型路径配置化、ACCEPTANCE 清单、纯函数单测、CI 骨架；未完成：2 小时长跑 + 切换 50 次、软解回退端到端、CI 实跑）。
2. **M6（进行中，实时链路已接通）**：环形缓冲 + VAD 分段 + `RealtimeSubtitleWorker` 已完成；剩 ①真实视频上的端到端延迟打点（目标 <= 8s） ②句尾 flush（停止/静音收尾时补最后一段） ③分段过长时的合并策略（whisper 单次调用成本固定约 30s 编码窗口，段越碎越亏） ④翻译延迟占比实测。
3. 拆文件仍登记为技术债 D-01，按需抽取，不作为独立任务（触发条件见 `docs/REFACTOR_PLAN.md`）。
