# AGENTS.md — 本仓库的工作约定

给"下一个进入这个仓库的人/模型"看的。先读本文件，再读 `PROGRESS.md`，最后按需读 `docs/`。

## 构建与运行

```powershell
# 配置（只需一次；Qt 路径固定）
cmake -B build -G "Visual Studio 18 2026" -A x64 -DQt6_DIR="C:/Qt/6.12.0/msvc2022_64/lib/cmake/Qt6"
# 增量构建（Debug 用于开发，Release 用于打包）
cmake --build build --config Debug -j 8
# 运行
.\build\Debug\MediaPlayer.exe
# 无 GUI 翻译回归（改 prompt/采样后必跑）
.\build\Debug\translate_cli.exe --model third_party\models\Hy-MT2-1.8B-Q4_K_M.gguf `
    --lang 中文 --input tests\translate_samples.txt
```

- `third_party/` 不进 git：模型、FFmpeg、SDL2、llama.cpp、whisper.cpp 都由 `scripts/fetch-deps.ps1` 拉取，版本见 `docs/THIRD_PARTY_VERSIONS.md`。
- 运行 exe 前若缺 `Qt6Cored.dll`，把 `C:\Qt\6.12.0\msvc2022_64\bin` 加进 `PATH`。
- `translate_cli` 的 stderr 会刷 llama 图构建日志，属正常输出。

## 代码规则

- **不要按行号定位代码**：先 `rg` 函数名再读。行号会随改动漂移，本文件与文档里不写行号。
- 线程与资源：任何 `QThread`/`std::thread` 在对象析构前必须 `wait()`；跨线程 lambda 不许 `[&]` 捕获局部变量。
- D3D11 设备引用计数：自建设备交给 FFmpeg 的 `AVHWDeviceContext` 前必须 `AddRef()`，FFmpeg 的 `d3d11va_device_uninit` 会无条件 Release。
- 条件变量：置停止标志必须在对应 mutex 内，再 `wakeAll()`，否则 worker 丢唤醒后会永久 `wait()`。
- 注释写"为什么"，不写"是什么"；中文注释，ASCII 标点。
- **NV12 纹理不能用 `D3D11_USAGE_DYNAMIC`**：`CreateTexture2D` 会返回 `E_INVALIDARG(0x80070057)`，软解一帧都上传不了、表现为"有声音黑屏"。CPU 上传只能走 `D3D11_USAGE_STAGING` + `Map(D3D11_MAP_WRITE)`，再 `CopySubresourceRegion` 到池里的 DEFAULT 纹理。改这块先跑 `build\Debug\d3d11_probe.exe`。
- **日志消息用英文/ASCII**：`LOG_*` 宏走 `QString::asprintf` 的 `%s`，中文会按 GBK 解码成乱码写进日志文件。注释可以中文，日志字符串不行。
- 新增可在无 GUI 下验证的逻辑，优先放进 `translate_cli` 这类命令行入口，而不是只挂在按钮上。
- 模型/资源路径不许硬拼 `../../third_party/`：走 `AppPaths::resolveResource()`。查找顺序：`<exe>/MediaPlayer.ini` 的 `[ai]` 段 > 注册表 `HKCU\Software\MediaPlayer\Config` > exe 周边约定路径 > `<exe>\models`。
- **"没有模型"和"模型正在加载"必须分开表达**：用 `main.cpp` 里的 `ModelState`（Missing/Loading/Ready/Failed）+ `AppPaths::modelsDirHint()` 提示应放位置。不要用 `isModelLoaded()` 单独判断——模型文件缺失时它永远是 false，UI 会一直显示"加载中"（v0.5.3 便携版就踩过这个）。
- **`.ps1` 脚本内容必须全 ASCII**：Windows PowerShell 5.1 把无 BOM 的脚本按 ANSI 读，中文会变乱码并报 `The string is missing the terminator`，整个脚本不执行。需要中文的文本（如 `README-START.txt`）单独放 `.txt` 由脚本复制，别写进 `.ps1`。
- **Silero VAD 流式推理前必须 `whisper_vad_reset_state()` 一次**：新分配的 backend buffer 不保证为 0，带着脏状态推理会系统性低估语音概率（实测 speech ratio 62.8% → 21.5%，断句全错）。之后用 `whisper_vad_detect_speech_no_reset()` 连续喂；seek/换片再 reset。

## 文档规则

- `PROGRESS.md` 是唯一可原地覆盖的文档，每次改动都要更新"当前状态"。
- 阶段总结/历史方案不改写，新写一份进 `docs/`，旧的留在 `docs/archive/`。
- 顶层只保留 `README.md`、`AGENTS.md`、`PROGRESS.md`、`ROADMAP.md`，其余进 `docs/`。

## 发布与便携包

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package.ps1   # -> dist\ + MediaPlayer-v<版本>-win64.zip
```

- 版本号只从 `Version.h` 读，zip 名、日志首行、关于框都跟着它走；发版先 bump 再打包。
- `dist\` 里带 `models\PUT-MODELS-HERE.txt` 与 `MediaPlayer.ini.example`：模型不进包（约 1.5GB），用户放 `models\` 或改 ini 写绝对路径。
- 便携包解压到任意目录都必须能跑：日志与崩溃报告写在 exe 同级的 `logs\`、`crash_reports\`，不要按 `../../` 推层级。

## 提交规则

- 一个提交只做一件事，信息用 `<type>: <一句话>`，正文写"根因"和"验证方式"。
- 每个可用版本打 tag：`v0.4.x`。Release 包由 `windeployqt` 生成后挂到 GitHub Releases。
- 不提交 `build/`、`third_party/`、`logs/`、`crash_reports/`、`picture/`、`*.dmp`。

## 测试

- 实时字幕链路自检：`build\Release\rt_cli.exe --wav <16k wav> --asr-model <ggml> --vad-model <silero> [--lang 中文]`，终端逐行打印 `[t0 - t1] proc lag 文本`。
- whisper 单次调用成本约等于一个 30s 编码窗口，与音频长短基本无关：tiny 约 0.6s/段，small 约 3.3s/段（8 线程）。所以段切得越碎越亏，实时链路要控制段数与段长。
- `tests/translate_samples.txt` 是翻译链路的固定语料（7 句，EN/JA/ZH/KO/FR）。
- 改 `Translator` 后必须跑一次 `translate_cli`，确认四语目标各 7/7 非空、无第二语言行。
- 崩溃排查：`crash_reports/*.txt` 现在含符号化调用栈（`模块!函数+偏移 (文件:行)`），先看栈再看 `.dmp`。
