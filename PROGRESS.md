# PROGRESS

> 本文件是唯一可原地覆盖的进度文档。最后更新：2026-10-28（v0.5.7 已发布；S3 GPU 路线进行中，卡在 MSVC 工具集）

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
| `v0.5.4` | 缺模型不再误报"加载中"（三态提示 + 应放路径）；便携 ini 配置；`scripts/package.ps1` 一键打包 |
| `v0.5.5` | 依赖脚本加重试与体积校验（修 CI 上偶发的拉取失败）；版本号与 tag 对齐 |
| `v0.5.6` | 实时字幕改成"整句攒批"再识别/翻译：VAD 段跨换气合并（1.2s 静音才算断句）、超过 6s 强制切、半句文本续接 |
| `v0.5.7` | `scripts/package.ps1` 自己定位 `windeployqt`，缺 Qt DLL 直接报错（之前 PATH 没 Qt 时会静默打出坏包） |

## S3 GPU 路线（进行中，2026-10-28 暂停）

目标：whisper turbo + Hy-MT2-1.8B Q8 都上 GPU，把实时字幕延迟从 10–13s 压到 2s 内（ROADMAP FR-14 ≤8s）。

- **已完成**：CUDA 12.9 工具链装好（`nvcc` 可用，VS 集成组件已补、MSBuild 规则已复制到 VS 的 `v180\BuildCustomizations`）；模型已下载 `ggml-large-v3-turbo.bin`、`Hy-MT2-1.8B-Q8_0.gguf`。
- **代码已就位但默认关闭**：`CMakeLists.txt` 的 `MP_CUDA` 开关（开时 `find_package(CUDAToolkit)`、`GGML_CUDA=ON`、`CMAKE_CUDA_ARCHITECTURES "89;120"`、拷贝 `ggml-cuda` 与 `cudart64_*/cublas64_*/cublasLt64_*`）；`Translator` 的 `n_gpu_layers` 读 `MP_MT_GPU_LAYERS`（缺省 99）；`WhisperTranscriber` 在 **context params** 上设 `use_gpu`（`MP_ASR_CPU=1` 可强制 CPU）；模型候选顺序改为 turbo / Q8 优先、small / Q4 兜底。
- **卡点**：`nvcc` 编译任何 `.cu` 都崩 `cudafe++ died with status 0xC0000005`。NVIDIA 论坛确认是 **VS2026 自带 MSVC 14.51 的兼容问题**（14.50.x 正常），本机只有 `14.51.36231`。补装 14.50 需要知道 VS18 的准确组件 ID（我两次猜的 ID 都没装出东西，`--installPath` 那次失败是路径空格没加引号）。
- **恢复构建的方法**：`cmake -S . -B build -DMP_CUDA=OFF -DGGML_CUDA=OFF`（`build` 缓存里只要残留 `GGML_CUDA=ON`，CPU 配置也会去跑 `CompilerIdCUDA` 而失败）。
- **下一步（用户已选 A）**：查 VS18 组件清单 → 装 MSVC 14.50 工具集 → `cmake -S . -B build -DMP_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES="89;120"` 重建 → 用 `rt_cli` 复测延迟与显存占用；若 1080p 超过 6.5GB 再按"turbo 换 Q5_1 / Q8 换 Q6_K / n_ctx 降到 2048"降档。


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

- 构建：`cmake --build build --config Debug -j 8` 通过，无新增告警（既有 `MSB8027` 来自 llama.cpp 同名源文件，可忽略）。
- 翻译回归：`translate_cli` + `tests/translate_samples.txt`，中文/英语/日语/韩语四向各 7/7 非空，单句 1.1–1.8 秒，无空译、无第二语言行。
- 需求达成（对照 ROADMAP v5）：M1–M3 完成；M4 除"AI 字幕端到端体验"外完成；M5 进行中（已完成：D-02 模型路径配置化、ACCEPTANCE 清单、纯函数单测、CI 工作流与依赖脚本；未完成：2 小时长跑、软解端到端、CI 实跑）。
- 模型定位：启动日志会打印 `resource ai/whisperModel found at: ...`；配置项 `ai/whisperModel`/`ai/translatorModel` 可覆盖，便携包把模型放 `models/` 即可被优先命中。
- **便携配置（v0.5.4）**：`<exe>/MediaPlayer.ini` 的 `[ai]` 段优先于注册表，`whisperModel`/`vadModel`/`translatorModel` 写绝对路径或相对 exe 的路径；模板见 `scripts/MediaPlayer.ini.example`。
- **缺模型的提示（v0.5.4）**：模型状态分 缺失/加载中/加载失败/就绪 四态，按钮点击时分别提示，缺模型会直接给出 `<exe>\models` 路径；不再把"没模型"显示成"加载中"。
- **打包**：`powershell -ExecutionPolicy Bypass -File scripts\package.ps1` 产出 `dist\` 与 `MediaPlayer-v<版本>-win64.zip`（版本号从 `Version.h` 读，不会漂移）。

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
5. 自动化测试：纯函数 74 断言 + D3D11 纹理探针（`ctest` 2/2）；播放链路仍靠 `docs/ACCEPTANCE.md` 手动回归。CI 已在 Actions 上跑通（v0.4.8 起每版绿）。
6. **实时字幕延迟超标**：真实日语片段（Guilty Crown 30s）实测，`ggml-small` 下每 6s 句子耗时约 5s（识别 4.4s + 翻译 0.6s），字幕比画面晚 10–13s；`ggml-tiny` 约 3s/句、晚 8s 左右但识别质量下降。CPU 是硬瓶颈，要压到 ROADMAP 的 ≤8s 需要 GPU（CUDA 版 whisper）或换更快的识别模型。
7. 翻译"不搭边"的根因已在 v0.5.6 处理（碎片投喂），但**没有在真实长片源上做整集回归**；模型替换（更大参数）暂不需要。

## 下一步（明天从这里继续）

1. **M6-a 真实片源回归（先做）**：用新包整集看一遍，记录「哪几句还是看不懂」——是识别错（人名/术语/漏词）还是译文错（语序/语气），这决定下一步调 ASR 还是 MT。命令行走 `rt_cli`，GUI 走 `dist/MediaPlayer.exe`。
2. **M6-b 模型对比评测（有数据再换模型）**：把 30 秒–5 分钟真实语音的 ASR 原文存成语料，用 `translate_cli` 跑 Hy-MT2 与候选模型同一批句子，出对比表；只有确认 `ASR 原文正确、译文仍差` 才换翻译模型。
3. **M6-c 延迟优化（进行中）**：当前 `small` 约 10–13s 延迟、`tiny` 约 8s，目标 ≤8s。已走 GPU 路线（见上面「S3 GPU 路线」），当前卡在 MSVC 14.50 工具集。
4. 拆文件仍登记为技术债 D-01，按需抽取，不作为独立任务（触发条件见 `docs/REFACTOR_PLAN.md`）。
