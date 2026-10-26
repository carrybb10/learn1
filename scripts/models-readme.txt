模型文件放在这个目录（发布包里不包含，需自己拷进来）：

  ggml-small.bin            语音识别（whisper），约 465 MB
  ggml-silero-v5.1.2.bin    语音端点检测（Silero VAD），约 0.85 MB
  Hy-MT2-1.8B-Q4_K_M.gguf   翻译，约 1.08 GB

没有模型时播放器仍可播放视频，但"AI 字幕"与"实时字幕"不可用，
点击按钮会提示缺失，并打印它去找过的路径。

不想拷贝模型：把上一层的 MediaPlayer.ini.example 改名为 MediaPlayer.ini，
在 [ai] 段写模型绝对路径即可。

下载地址见仓库 docs/THIRD_PARTY_VERSIONS.md（含每个文件的大小与用途）。
