# PROGRESS

> 本文件是唯一可原地覆盖的进度文档。最后更新：2026-10-28（v0.5.8 已发布；S3 GPU 路线已打通，实时字幕延迟降到 1.4–2.8s）

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
| `v0.5.1` | Silero VAD 分段链路打通：`SileroVad` + `VadSegmenter` + `vad_cli`，流式与整段结果一致（CI run 40337277482 通过） |
| `v0.5.2` | 实时字幕接通播放链路：`RealtimeSubtitleWorker`（环形缓冲 -> VAD -> whisper -> 翻译）+ `rt_cli` 端到端工具；whisper 线程数随核数提升 |
| `v0.5.3` | 便携版把 `logs/`、`crash_reports/` 留在自己目录（原来写到上一级），重新打包 |
| `v0.5.4` | 缺模型不再误报"加载中"（三态提示 + 应放路径）；便携 ini 配置；`scripts/package.ps1` 一键打包 |
| `v0.5.5` | 依赖脚本加重试与体积校验（修 CI 上偶发的拉取失败）；版本号与 tag 对齐 |
| `v0.5.6` | 实时字幕改成"整句攒批"再识别/翻译：VAD 段跨换气合并（1.2s 静音才算断句）、超过 6s 强制切、半句文本续接 |
| `v0.5.7` | `scripts/package.ps1` 自己定位 `windeployqt`，缺 Qt DLL 直接报错（之前 PATH 没 Qt 时会静默打出坏包） |
| `v0.5.8` | CUDA 构建打通（`build-cuda`）；VAD 吞吐修复（每 960ms 调一次，30s 音频 260→31 次调用）；翻译指令改到 user + 官方采样参数 + 风格约束；GPU 版打包 |
| `v0.5.9` | 上下文翻译（T6.4）：前 2 句原文+译文带进 prompt，回显自动退回无语境重翻；`translate_cli --context-lines` 做 A/B；断言 74→86 |

## S3 GPU 路线（已完成，v0.5.8）

目标：whisper turbo + Hy-MT2-1.8B Q8 都上 GPU，把实时字幕延迟从 10–13s 压到 2s 级（ROADMAP FR-14 ≤8s）。

- **结果**：30s 日语片段（turbo + silero + Q8）4 条字幕、`lag 1.36–2.84s`、ASR+翻译 0.3–0.8s/句；显存占用约 4GB（8GB 卡余量充足）。
- **工具链**：CUDA 12.9 + **MSVC 14.50**（`14.50.35717`）。VS2026 自带的 14.51 会让 `cudafe++` 崩 `0xC0000005`，必须退到 14.50。
- **构建命令**（独立目录，不影响 CPU 的 `build\`）：
  ```powershell
  $env:CUDA_PATH_V12_9 = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9'
  cmake -S . -B build-cuda -G "Visual Studio 18 2026" -A x64 -T "v145,version=14.50.35717" `
        -DMP_CUDA=ON -DCMAKE_CUDA_FLAGS="-allow-unsupported-compiler" `
        -DQt6_DIR="C:/Qt/6.12.0/msvc2022_64/lib/cmake/Qt6"
  cmake --build build-cuda --config Release -j 8
  ```
  注意：工具集名是 `v145`；MSBuild 的 CUDA 规则读的是 `CUDA_PATH_V12_9`（不是 `CUDA_PATH`）；`-allow-unsupported-compiler` 是必需的。
- **打包**：`scripts\package.ps1 -Gpu` 产出 `dist\` 与 `MediaPlayer-v<版本>-win64-cuda.zip`，并校验 `ggml-cuda.dll / cudart64_12.dll / cublas64_12.dll` 都在。
- **运行期开关**：`MP_ASR_CPU=1` 强制 ASR 走 CPU；`MP_MT_GPU_LAYERS=<n>` 控制翻译层上 GPU 的张量层数（默认 99）；`MP_VAD_THREADS=<n>` 覆盖 VAD 线程数（默认核数/2，实测 1–4 线程比默认更快）。


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
- **M6 实时字幕进度**：环形缓冲（60s、3.84MB 固定占用）→ VAD 分段 → whisper → 翻译已接通播放链路（`RealtimeSubtitleWorker`，v0.5.2），并有无 GUI 端到端工具 `rt_cli`。
- **v0.5.6 翻译质量修复**：症状是「翻译基本不搭边」。取证结论：模型本身没问题，是**投喂的音频碎片**——VAD 在换气处断开后只有 1–3 秒，whisper 会丢词/幻觉（`ask not what your country` → 识别成 `What country can do for you?` → 译成「哪个国家能为你服务？」）。修复：VAD 段跨换气合并成整句（静音 ≥1.2s 才算断句）、最长 6s 强制切、识别结果若是半句先攒着等拼成完整句子再翻译。

- **需求达成（对照 ROADMAP v5，截至 v0.5.9）**：M1–M4 完成；**M5 收尾中**（已完成：D-02 路径配置化、ACCEPTANCE 清单、86 条纯函数断言、CI 实跑；未完成：2 小时长跑只跑到 105 分钟、软解无真实 4K 片源回归、计划点名的 6 组单测未写、内存基线未记）；**M6 完成约 4/5**（环形缓冲 / Silero VAD / 分段 ASR / 上下文翻译已完成，GUI 延迟打点、流式未做）。
- **v0.5.9 上下文翻译（T6.4）**：`RealtimeSubtitleWorker` 保留最近 2 句（原文+译文）喂给 `Translator::translateWithContext`；上文块放在原文**之后**，因为放在前面时模型会去翻译提示语本身（实测 4 条字幕里 3 条输出的是提示语）；`looksLikePromptEcho()` 命中提示语特征时自动丢弃语境重翻一次，保证不会比无语境更差。
- **上下文收益（实测，不夸大）**：gc30.wav 30s 片段带/不带上下文都是 4 条、文本只差语气词，无语境回归；7 句固定语料 A/B 有两处细微差异（"我们得掉头回去"→"我们得回去了"），单句慢 0.5–1.0s（Debug/CPU）。**收益要在长对话片源上才能体现，尚未验证**。
- **v0.5.8 实时字幕现状（GPU）**：30s 日语片段 4 条字幕、延迟 1.36–2.84s、ASR+翻译 0.3–0.8s/句；VAD 从"每 100ms 调一次"改为"攒满 960ms 调一次"，同等音频调用数 260 → 31。
- 构建：CPU 走 `cmake --build build --config Debug -j 8`；GPU 走 `build-cuda`（命令见上一节），两者互不影响。
- 翻译回归：`translate_cli` + `tests/translate_samples.txt`，中文/英语/日语/韩语四向各 7/7 非空，单句 97–166ms（Q8 + GPU），无空译、无第二语言行。
- 模型定位：启动日志会打印 `resource ai/whisperModel found at: ...`；配置项 `ai/whisperModel`/`ai/translatorModel` 可覆盖，便携包把模型放 `models/` 即可被优先命中。
- **便携配置（v0.5.4）**：`<exe>/MediaPlayer.ini` 的 `[ai]` 段优先于注册表，`whisperModel`/`vadModel`/`translatorModel` 写绝对路径或相对 exe 的路径；模板见 `scripts/MediaPlayer.ini.example`。
- **缺模型的提示（v0.5.4）**：模型状态分 缺失/加载中/加载失败/就绪 四态，按钮点击时分别提示，缺模型会直接给出 `<exe>\models` 路径；不再把"没模型"显示成"加载中"。
- **打包**：`scripts\package.ps1` 打 CPU 包（`MediaPlayer-v<版本>-win64.zip`），`scripts\package.ps1 -Gpu` 打 GPU 包（`...-win64-cuda.zip`，会校验 CUDA 三个 DLL 存在）；版本号从 `Version.h` 读，不会漂移。

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
5. 自动化测试：纯函数 86 断言 + D3D11 纹理探针（`ctest` 2/2）；播放链路仍靠 `docs/ACCEPTANCE.md` 手动回归。CI 已在 Actions 上跑通（v0.4.8 起每版绿）。
6. **实时字幕延迟（已从 10–13s 降到 1.4–2.8s）**：GPU 版（turbo + Q8）达标；CPU 版仍是 10s+，官方包默认走 CPU，用户机器没有 NVIDIA 卡时会退化到 CPU 体验。发行说明里要写清这一点。
7. 翻译"不搭边"的根因已在 v0.5.6 处理（碎片投喂），v0.5.8 又调了指令位置与风格约束；但**没有在真实长片源上做整集回归**。已知副作用：风格约束让 `あくび` 从"打哈欠"变成"打瞌睡"，属准确率回退，待补"不得改变具体动作含义"的约束。
8. **停止时线程归属警告**：`rt_cli` 与 GUI 退出偶现 `QObject::killTimer: Timers cannot be stopped from another thread` —— `requestStop()` 投递到 worker 线程的 `stop` 没有按预期执行，待查（不影响结果，但属于未定义行为）。
9. **VAD 与 ASR 在同一线程串行**：`lag` 里有一部分是排队时间；把 VAD 拆到独立线程或做流水线是 M6 的优化项。

## 下一步（按顺序，做完一条再动下一条）

1. **M6-a 真实片源回归**：用 `-Gpu` 包整集看一遍，记录"哪几句还是看不懂"，区分识别错（人名/术语/漏词）与译文错（语序/语气）；同时验证上下文翻译（T6.4）是否真的改善了跨句指代。
2. ~~**M6-b 上下文翻译（T6.4）**~~ — 已在 v0.5.9 完成（见上）。
3. **M6-c GUI 延迟打点（T6.5）**：把 `lag` 显示到字幕面板或诊断面板，替代现在只能看日志。
4. **M6 收尾**：修 `killTimer` 线程归属、VAD 与 ASR 解耦、补 `あくび` 那类准确率约束，然后打 `v0.6.0`。
5. **M5 补测**：计划点名的 6 组单测（`avToSdlFormat`/音量缩放/seek 换算/时钟插值/WAV 头/播放列表）+ 2 小时长跑 + 内存基线。
6. 拆文件仍登记为技术债 D-01，按需抽取（触发条件见 `docs/REFACTOR_PLAN.md`）。

## 收尾检查点（2026-10-28）

- 当前版本 `v0.5.9`，`HEAD = 3bf5e5d`，已推送；CI run #15 success；工作区干净。
- 今天产出的包：`MediaPlayer-v0.5.9-win64.zip`（79.3MB）、`MediaPlayer-v0.5.9-win64-cuda.zip`（678.7MB）。
- 正在用的 GPU 测试目录：`D:\new\test\MediaPlayer-gpu\`（exe 已是 v0.5.9，`MediaPlayer.ini` 指向 turbo + Q8 + silero）。
- 源码快照：`D:\new\_backup\Player_src_v0.5.9_20261028.zip`；全量备份 `Player_20261025_1717` 未动。
- 明天从"下一步"第 1 条开始：先看片收集问题，再决定 M6-c（延迟打点）与 M6 收尾的先后。
- 回归素材：`D:\new\test\gc30.wav`（30s 日语片段，用于 `rt_cli` 对照）。

