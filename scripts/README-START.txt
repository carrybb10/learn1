MediaPlayer 便携版 —— 解压即用
================================

0) 先看这里：本包不含 AI 模型
   AI 字幕（离线识别/实时字幕）需要三个模型文件，放在本目录的 models\ 里：
     ggml-small.bin           ~465 MB   语音识别
     ggml-silero-v5.1.2.bin   ~0.85 MB  语音端点检测
     Hy-MT2-1.8B-Q4_K_M.gguf  ~1.08 GB  翻译
   没有模型时播放器照常播放视频，但点"AI 字幕"会提示找不到模型，并给出应放的路径。

   不想把模型拷进来？用本目录的 MediaPlayer.ini 写绝对路径即可（改名去掉 .example）：
     [ai]
     whisperModel=D:/models/ggml-small.bin
     translatorModel=D:/models/Hy-MT2-1.8B-Q4_K_M.gguf
     vadModel=D:/models/ggml-silero-v5.1.2.bin
   启动日志会打印每个资源配置命中的路径，排查时先看日志。

1) 运行
   双击 MediaPlayer.exe。建议用 Release 版跑实时字幕：Debug 版慢一个数量级。

2) 日志与崩溃报告（都在本目录下，不写系统目录）
   logs\mediaplayer_*.log      运行日志
   crash_reports\*.txt         崩溃时含符号化调用栈；同名 .dmp 可用 WinDbg 打开

3) 依赖
   Qt 6.12.0、FFmpeg（LGPL shared）、SDL2、llama.cpp、whisper.cpp 已随包提供。
   Windows 10/11 x64，需要 D3D11（软件解码回退可用，但 4K 建议硬解）。

4) 已知限制
   实时字幕延迟约 2-4 秒（small 模型）；翻译模型加载约 3-5 秒；
   首次启动加载 VAD 与 ASR 需要几秒，按钮会提示"加载中"。
