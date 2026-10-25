# MediaPlayer 项目进度文档

> 最后更新：2026-10-03 | M4 T4.1-T4.7完成，T4.8离线AI翻译字幕集成完成，T4.9高效反馈系统完成

## 项目路径
`D:\new\Player\`

## 技术栈
- Qt 6.12.0 (msvc2022_64)
- FFmpeg (gyan.dev full-shared 版本，MSVC 编译)
- SDL2 2.32.4 (VC版本)
- D3D11 (零拷贝渲染)
- CMake + Visual Studio 2026 (MSVC v144)

## 当前架构：三线程分离 + D3D11 零拷贝渲染 + 独立UI窗口
```
读包线程(demuxLoop)
  ├─ 音频包 → 音频包队列 → 音频解码线程(audioDecodeLoop) → 音频帧队列 → SDL回调播放(软件音量缩放)
  └─ 视频包 → 视频包队列 → 视频解码线程(videoDecodeLoop)
                → D3D11VA硬解/软解回退 → GPU内拷贝到中转纹理池 → 视频帧队列(ID3D11Texture2D*+PTS)
                → 主线程QTimer(1ms PreciseTimer) PTS同步 → D3D11渲染(QWindow+交换链, Present(0,0)关垂直同步)

UI层：主窗口(纯视频) + 独立uiWindow(设置栏+控制栏+播放列表面板)，悬浮在主窗口底部，渐入渐出
字幕层：独立可拖动悬浮窗(DraggableLabel)，字幕开关与文件选择按钮分离
```

## 当前策略：策略2（可配置质量，手动切换）
- 硬件解码：D3D11VA 开启 / 关闭(纯软件) — 界面下拉框手动切换
- 输出分辨率：原分辨率 / 1080p / 720p — 界面下拉框手动切换（不做自动降画质）
- 音频：无损路径（动态匹配源采样率/格式/声道，planar→packed 无重采样）
  - SDL 支持源格式时直接直通输出（无损）
  - SDL 不支持时自动回退 44100/S16 重采样（兼容）
- 音量：软件 PCM 缩放（支持 S16/FLT/S32，带削波保护）

## 已实现功能
### M1 基础播放
- [x] 打开视频文件（mp4/mkv/avi/flv/mov/wmv/webm）
- [x] FFmpeg 音视频解码（D3D11VA 硬件 + 软件回退）
- [x] SDL2 音频输出（无损直通 + 回退重采样双路径）
- [x] 播放/暂停控制
- [x] 进度条显示（基于音频时钟插值 + PTS 同步）
- [x] 三线程架构（读包/音频解码/视频解码分离）
- [x] 视频帧队列背压机制（队列满时等待，不丢帧）
- [x] 空文件保护（未打开文件时点播放不崩溃）

### M2 D3D11 零拷贝渲染
- [x] D3D11 零拷贝渲染（QWindow + 原生交换链 + NV12→RGB BT.709 着色器）
- [x] 中转纹理池（预分配循环使用，避免每帧 GPU 内存分配）
- [x] D3D11VA lock/unlock 回调（立即上下文线程安全）
- [x] D3D11 设备全局复用（切换视频不重建设备，避免跨设备纹理闪退）
- [x] 4K 60fps 原分辨率稳定播放（decodeFps=60, displayFps=60, frameQueue=9）

### M3 交互功能
- [x] 进度条拖拽 seek（停止线程→av_seek_frame+flush→清空队列→重启线程，毫秒级精度）
- [x] 音量控制（软件 PCM 缩放，控制栏滑条 0-100，支持 S16/FLT/S32）
- [x] 全屏播放（F/ESC 切换，控制栏渐入渐出，鼠标移动唤起）
- [x] 设置持久化（QSettings 保存硬件解码/分辨率/音量，启动恢复）
- [x] 文件末尾乱码保护（demux 结束 flush 标记，队列空才保持上一帧）
- [x] 键盘快捷键（空格播放暂停、←→ seek 5秒、↑↓ 音量±5、F 全屏、ESC 退出）
- [x] 独立 UI 窗口方案（设置栏+控制栏移到独立窗口，解决多层叠加/重影）
- [x] 全屏无白色矩形（渲染器延迟到第一帧初始化，无视频时GDI填充黑色背景）
- [x] 切换视频稳定性（D3D11 设备全局复用，stop 只释放解码器）

### M4 高级功能
- [x] T4.1 软解回退（sws_scale转NV12→动态纹理CPU写入→CopySubresourceRegion到中转纹理）
- [x] T4.2 发布打包（WIN32_EXECUTABLE仅Release + QT_NO_DEBUG_OUTPUT，无控制台窗口）
- [x] T4.3 高帧率（显示定时器1ms PreciseTimer + Present(0,0)关闭垂直同步）
- [x] T4.4 字幕（SubtitleParser支持SRT/ASS/SSA，可拖动悬浮窗，开关与文件选择分离）
- [x] T4.5 播放列表（添加/移除/双击播放/自动连播，播放列表面板可切换）
- [x] T4.6 截图（D3D11 back buffer回读→staging→Map→QImage，保存到picture文件夹，淡入淡出通知）
- [x] T4.7 拖拽打开文件（主窗口+central+videoWidget均接受拖拽，多文件自动加入播放列表）
- [x] T4.8 离线AI翻译字幕（Phase 1完成：whisper.cpp ASR + HY-MT2-1.8B翻译，支持日/英/韩/中，完全离线，模型1.06GB）
- [x] T4.9 高效反馈系统（结构化日志+崩溃捕获+实时诊断面板Ctrl+D+一键导出诊断包Ctrl+E）

## 关键文件
- `main.cpp` — 主窗口(纯视频) + 独立uiWindow(设置栏+控制栏+播放列表面板)、全屏逻辑、快捷键、设置持久化、字幕悬浮窗、截图、拖拽
- `PlayerController.h/cpp` — 核心控制器：三线程解码 + D3D11VA+软解回退 + 音频时钟插值 + PTS同步 + seek + 软件音量 + 播放列表
- `VideoWidget.h/cpp` — 视频窗口（QWindow，expose时初始化渲染器，GDI黑色背景，截图接口）
- `D3D11Renderer.h/cpp` — D3D11 渲染器：交换链/着色器/中转纹理池(StagingTexturePool)/全局锁(g_d3d11Mutex)/截图回读
- `SubtitleParser.h/cpp` — 字幕解析器（SRT/ASS/SSA）
- `CMakeLists.txt` — 构建配置（WIN32_EXECUTABLE仅Release、QT_NO_DEBUG_OUTPUT、链接 d3d11/dxgi/d3dcompiler、POST_BUILD拷贝DLL）
- `third_party/ffmpeg/` — FFmpeg开发包（include/lib/bin）
- `third_party/SDL2/` — SDL2开发包（include/lib）
- `picture/` — 截图保存目录
- `M1_DEVELOPMENT_SUMMARY.md` — M1阶段开发总结
- `M2_DEVELOPMENT_SUMMARY.md` — M2阶段开发总结
- `M3_DEVELOPMENT_SUMMARY.md` — M3阶段开发总结
- `M4_DEVELOPMENT_SUMMARY.md` — M4阶段开发总结
- `ROADMAP.md` — 开发大纲 v1
- `ROADMAP_v2.md` — 开发大纲 v2（M2完成）
- `ROADMAP_v3.md` — 开发大纲 v3（M3完成）
- `ROADMAP_v4.md` — 开发大纲 v4（M4完成）
- `PROGRESS.md` — 本项目进度文档

## 编译命令
```powershell
cd D:\new\Player
cmake -B build -G "Visual Studio 18 2026" -A x64 -DQt6_DIR="C:/Qt/6.12.0/msvc2022_64/lib/cmake/Qt6"
cmake --build build --config Release -j
# 首次或清理build后需要部署Qt依赖：
C:/Qt/6.12.0/msvc2022_64/bin/windeployqt.exe build/Release/MediaPlayer.exe
# 运行：
./build/Release/MediaPlayer.exe
# Debug诊断模式（输出qDebug到日志）：
& "D:\new\Player\build\Debug\MediaPlayer.exe" 2>&1 | Tee-Object D:\new\Player\diag.log
```

## 已知问题
1. 个别关键帧间隔过长的视频，seek 到末尾几秒时恢复较慢（视频本身特性，不影响大部分视频）
2. 软解回退路径已实现但未充分端到端验证（硬解模式正常）

## 重要代码注意事项
- `avcodec_send_packet` 必须检查返回值，EAGAIN时先receive_frame再重发（否则乱码）
- `av_opt_set_chlayout` 是FFmpeg 7.x+新API，旧版用`av_opt_set_channel_layout`
- SDL头文件直接在include/下，用`#include <SDL.h>`而非`#include <SDL2/SDL.h>`
- `SDL_MAIN_HANDLED`编译定义必须设置，否则SDL会重定义main导致链接错误
- 视频包队列满时不能阻塞读包线程，否则音频会中断
- 无损音频：目标格式=源packed格式，采样率/声道数不变，swr仅做planar→packed
- **D3D11 必须用 QWindow 不能用 QWidget**（backing store 会覆盖渲染结果）
- **D3D11VA 解码纹理 BindFlags 只有 DECODER**，必须拷贝到中转纹理才能创建 SRV
- **必须自己创建 D3D11 设备传给 FFmpeg**（FFmpeg 自动创建设备不支持显示输出）
- **必须禁用背面剔除**（D3D11_CULL_NONE），否则全屏四边形被剔除
- **解码纹理必须在解码线程立即拷贝**（解码器复用纹理数组，AddRef 不能阻止覆盖）
- **必须设置 AVD3D11VADeviceContext.lock/unlock 回调**（立即上下文线程安全）
- **中转纹理用池管理**（StagingTexturePool，acquire/release），避免每帧分配释放
- **D3D11 设备必须全局复用**（切换视频不重建设备，否则跨设备纹理闪退）
- **UI 必须用独立窗口**（Qt子控件与createWindowContainer混合渲染会导致多层叠加/重影/白屏）
- **渲染器必须延迟到第一帧初始化**（用解码设备，避免设备切换导致交换链重建失败）
- **无视频时必须用GDI填充黑色背景**（QWindow默认白色，QSS对其无效）
- **swap chain 格式是 R8G8B8A8_UNORM**，截图必须用 QImage::Format_RGBA8888（用ARGB32会R/B颠倒）
- **拖拽必须在 window/central/videoWidget 三处都安装 eventFilter + setAcceptDrops**（createWindowContainer容器会拦截事件）
- 音频时钟用 SDL 已播放样本数 + 系统时钟插值（100ms上限，音频结束后不无限推进）
- seek 后快速跳帧必须在纹理拷贝之前检查 PTS（否则不需要的帧也做昂贵的GPU拷贝）
- CMake Release 用 WIN32_EXECUTABLE + QT_NO_DEBUG_OUTPUT 去掉控制台
- 全屏时 UI 窗口用渐入渐出动画（QPropertyAnimation windowOpacity），不影响视频比例
