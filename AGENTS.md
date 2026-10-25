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
- 新增可在无 GUI 下验证的逻辑，优先放进 `translate_cli` 这类命令行入口，而不是只挂在按钮上。

## 文档规则

- `PROGRESS.md` 是唯一可原地覆盖的文档，每次改动都要更新"当前状态"。
- 阶段总结/历史方案不改写，新写一份进 `docs/`，旧的留在 `docs/archive/`。
- 顶层只保留 `README.md`、`AGENTS.md`、`PROGRESS.md`、`ROADMAP.md`，其余进 `docs/`。

## 提交规则

- 一个提交只做一件事，信息用 `<type>: <一句话>`，正文写"根因"和"验证方式"。
- 每个可用版本打 tag：`v0.4.x`。Release 包由 `windeployqt` 生成后挂到 GitHub Releases。
- 不提交 `build/`、`third_party/`、`logs/`、`crash_reports/`、`picture/`、`*.dmp`。

## 测试

- `tests/translate_samples.txt` 是翻译链路的固定语料（7 句，EN/JA/ZH/KO/FR）。
- 改 `Translator` 后必须跑一次 `translate_cli`，确认四语目标各 7/7 非空、无第二语言行。
- 崩溃排查：`crash_reports/*.txt` 现在含符号化调用栈（`模块!函数+偏移 (文件:行)`），先看栈再看 `.dmp`。
