# 预翻译路线原型（experiment）

目标：验证"打开视频前先把整篇字幕做好，播放时只渲染"这条路是否可行、值不值得做。
结论先行：**可行，且在你的机器上 24 分钟片源约 1–1.5 分钟出字幕。** 是否并入主线由你决定。

## 原型

`tools/pretranslate_cli.cpp`（分支 `exp/pretranslate`，未合并进 `main`）

```powershell
# CPU 构建（Debug/Release 均可）
cmake --build build --config Release -j 8 --target pretranslate_cli
$env:MP_ASR_CPU = '1'   # 非 CUDA 构建必须设，否则 whisper 会尝试无后端 GPU

# GPU 构建
cmake --build build-cuda --config Release -j 8 --target pretranslate_cli

.\build-cuda\Release\pretranslate_cli.exe `
  --input <视频或音频> --out <输出.srt> `
  --asr-model third_party\whisper.cpp\models\ggml-large-v3-turbo.bin `
  --vad-model third_party\whisper.cpp\models\ggml-silero-v5.1.2.bin `
  --asr-lang ja `
  --mt-model third_party\models\Hy-MT2-1.8B-Q8_0.gguf --lang 中文
```

链路：FFmpeg 解码任何容器 → Silero VAD 找语音区间 → 合并成 ≤25s 的识别窗口 →
whisper（逐窗，时间戳加回偏移）→ 相邻短段合并成句子级 → 按标点/字数断句 →
按语速估算每条时长 → `translateWithContext` 批量翻译（带最近 2 条上下文）→ SRT。

## 实测数字

| 素材 | 构建 | 解码 | VAD | ASR | 翻译 | 合计 |
|---|---|---|---|---|---|---|
| 30s 日语 | CPU Release（8 线程） | 7ms | — | 17.9s（0.60x 实时） | 9.9s / 7 条 | ~30s |
| 240s 日语 | GPU（turbo + Q8） | 26ms | 0.7s（78 窗口→9 块） | **2.9s（0.012x 实时）** | **8.2s / 64 条** | **14.2s** |

按 4 分钟 14.2s 外推 **24 分钟片源 ≈ 85s**（ASR ~17s + MT ~49s + VAD ~4s + 解码 <1s）。
首次观看有等待，第二次直接读 SRT 为 0。

质量示例（`gc240.ja-zh.srt`）：`早上好，顺，怎么了？时间好晚啊。` / `嗯，我刚才在网上闲逛了一下吧？` / `读书？`
平均每条 11.4 字，粒度接近正常字幕。

## 原型阶段修掉的三个问题（都会在正式版重现，记下来）

1. `swr_convert` 的输出参数是 `uint8_t **`（数组地址），把 `float*` 强转过去会让 swr 把前 8 字节当指针解引用 → `0xC0000005`。
2. `AVChannelLayout` 必须零初始化，`av_channel_layout_copy` 会先 `uninit` 目标，未初始化即野指针。
3. 非 CUDA 构建下 whisper 仍默认 `use_gpu=true`，必须用 `MP_ASR_CPU=1`（或让 CLI 读构建宏自动关掉）。

## 与实时路线相比的取舍

- 收益：整篇上下文、术语可固定、不会因积压丢句、可校对、可只重跑翻译（原文可缓存）。
- 代价：首次等待 1–1.5 分钟（要有进度与取消）；字幕与画面误差仍来自 whisper 段边界；
  片源换音轨要重新生成。
- 共享件：`SileroVad` / `VadSegmenter` / `WhisperTranscriber` / `Translator` 全部复用，实时模式不受影响。

## 已知问题（并入主线前要处理）

1. **ASR 误听**：人名（`シェン`→`顺`/`勋`）、`うろついてた`→`もろついてた`。CPU 与 GPU 结果略有差异，属模型能力上限，靠术语表/更大模型解决。
2. **日语无标点**：whisper 的 ja 输出不带句读，断句只能靠"相邻段合并 + 字数上限"，所以要补一套基于停顿和语法线索的断句规则。
3. **时间轴是近似值**：条目起点用 whisper 段起点，时长按语速估算。要精确到字，需要开 token 级时间戳（`whisper_full_params.token_timestamps`）。
4. **`translateWithContext` 新增 `sourceLang` 参数**：不传时行为不变；传了且与目标语言不同，就不再按字符集判断"已是目标语言"而透传——否则日语纯汉字短语（`読書`）会原样输出。
5. **没有缓存/断点续跑**：长片源中断要重跑，正式版要按 `(文件大小, mtime, 音轨, 模型, 参数版本)` 落盘。

## 下一步（若确认并入）

1. 把 `decodeAudio` + `buildCues` 抽成 `BatchSubtitlePipeline`（Qt 线程、进度信号、可取消）。
2. GUI：打开文件后显示进度（第 i/N 段、预计剩余），完成后自动 `loadSubtitle`；失败回退实时模式。
3. 断句/行长/CPS 规则抽成纯函数并补单测（当前在 CLI 里，逻辑未测）。
4. 缓存与偏移设置（±5s 全局偏移）。
5. 多音轨选择：`--audio-index`。
