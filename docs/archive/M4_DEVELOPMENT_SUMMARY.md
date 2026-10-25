# M4 高级功能开发总结

> 完成日期：2026-10-02
> 阶段目标：在 M3 交互功能基础上，实现高级播放功能（软解回退、发布打包、高帧率、字幕、播放列表、截图、拖拽打开文件）

## 任务清单与完成状态

| 任务 | 描述 | 状态 |
|------|------|------|
| T4.1 | 软件解码回退路径适配 D3D11 渲染 | ✅ 完成 |
| T4.2 | 发布打包（去控制台、Windows子系统） | ✅ 完成 |
| T4.3 | 高帧率视频支持（关闭垂直同步） | ✅ 完成 |
| T4.4 | 字幕支持（SRT/ASS，可拖动悬浮窗） | ✅ 完成 |
| T4.5 | 播放列表/连播 | ✅ 完成 |
| T4.6 | 截图功能 | ✅ 完成 |
| T4.7 | 拖拽打开文件 | ✅ 完成 |

---

## T4.1 软解回退

### 实现方案
- 新增 `ensureSwUploadTexture(int width, int height)`：创建 D3D11_USAGE_DYNAMIC + CPU_ACCESS_WRITE 的 NV12 动态纹理
- 新增 `uploadSoftwareFrame(AVFrame *frame, ID3D11Texture2D *&outStagingTex)`：
  1. `sws_scale` 将软解输出的 YUV420P 转为 NV12
  2. `Map` 动态纹理，按行拷贝 Y 平面和 UV 平面（注意行对齐）
  3. `CopySubresourceRegion` 从动态纹理拷贝到中转纹理池纹理
- 视频解码线程中统一处理：`frame->format == AV_PIX_FMT_D3D11` 走硬解路径，否则走软解路径

### 关键代码
```cpp
// PlayerController.cpp - uploadSoftwareFrame
if (!ensureSwUploadTexture(frame->width, frame->height)) return false;
D3D11_MAPPED_SUBRESOURCE mapped;
m_d3d11Context->Map(m_swUploadTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
// 拷贝Y平面
uint8_t *dstY = (uint8_t*)mapped.pData;
for (int y = 0; y < frame->height; y++)
    memcpy(dstY + y * mapped.RowPitch, frame->data[0] + y * frame->linesize[0], frame->width);
// 拷贝UV平面（NV12交错格式）
uint8_t *dstUV = dstY + mapped.RowPitch * frame->height;
// ... sws_scale 或手动交错拷贝
m_d3d11Context->Unmap(m_swUploadTexture, 0);
// 拷贝到中转纹理
outStagingTex = StagingTexturePool::instance().acquire(m_d3d11Device, frame->width, frame->height);
m_d3d11Context->CopySubresourceRegion(outStagingTex, 0, 0, 0, 0, m_swUploadTexture, 0, nullptr);
```

---

## T4.2 发布打包

### 实现方案
- CMakeLists.txt 中仅 Release 配置设置 `WIN32_EXECUTABLE`（无控制台窗口）
- 仅 Release 配置添加 `QT_NO_DEBUG_OUTPUT` 编译定义（去掉 qDebug 输出）
- Debug 配置保留控制台和诊断输出，便于开发调试

### CMake 配置
```cmake
set_target_properties(MediaPlayer PROPERTIES
    WIN32_EXECUTABLE $<CONFIG:Release>
)
target_compile_definitions(MediaPlayer PRIVATE
    $<$<CONFIG:Release>:QT_NO_DEBUG_OUTPUT>
)
```

---

## T4.3 高帧率

### 实现方案
- 显示定时器从 10ms 改为 1ms，使用 `Qt::PreciseTimer`（高精度定时器）
- `Present(1, 0)` 改为 `Present(0, 0)`，关闭垂直同步（第一个参数 0 = 不等待垂直同步）
- 这样显示器刷新率不再限制视频帧率，60fps/120fps/240fps 视频均可原帧率输出

### 关键修改
```cpp
// PlayerController.cpp
m_displayTimer = new QTimer(this);
m_displayTimer->setTimerType(Qt::PreciseTimer);
m_displayTimer->start(1);  // 1ms 精度

// D3D11Renderer.cpp
m_swapChain->Present(0, 0);  // 关闭垂直同步
```

---

## T4.4 字幕

### 实现方案
- 新增 `SubtitleParser` 类，支持 SRT/ASS/SSA 格式解析
- 字幕标签使用独立悬浮窗（`Qt::Tool | Qt::FramelessWindowHint | Qt::WA_TranslucentBackground`），避免被视频窗口裁剪
- 自定义 `DraggableLabel`：支持鼠标拖动，拖动时不透明背景减少重绘，拖动结束恢复半透明
- UI 设计：字幕开关（蓝色高亮 checkable 按钮）与文件选择按钮（精简"..."小按钮）分离
- 加载字幕后始终显示标签（无字幕内容时显示 32px 空条），保证可拖动区域存在

### SubtitleParser 核心逻辑
```cpp
struct SubtitleCue {
    double startTime;  // 秒
    double endTime;
    QString text;
};
// SRT: 解析 "00:00:01,000 --> 00:00:04,000" 格式
// ASS: 解析 "Dialogue: 0,0:00:01.00,0:00:04.00,..." 格式
QString getCueAtTime(double time) const;  // 返回当前时间应显示的字幕
```

### 遇到的问题
- **字幕被视频遮挡**：字幕标签作为主窗口子控件时会被 D3D11 渲染覆盖 → 改为独立 Tool 窗口
- **无字幕时标签消失无法拖动**：加载后始终显示，空内容显示固定高度空条

---

## T4.5 播放列表

### 实现方案
- PlayerController 新增播放列表管理：
  - `setPlaylist/addToPlaylist/removeFromPlaylist/playIndex/playNext/playPrevious`
  - `playlistChanged/currentIndexChanged/playbackFinished` 信号
- 播放结束检测：`demux结束 + 视频帧队列空 + 音频帧队列空` → 发出 `playbackFinished`
- UI：设置栏新增"列表"切换按钮，播放列表面板包含 QListWidget + 添加/移除按钮
- 双击列表项播放，播放结束自动播放下一个
- 打开文件时自动加入播放列表

### 播放结束检测
```cpp
// PlayerController::displayFrame()
if (m_videoFrameQueue.isEmpty()) {
    if (m_demuxFinished && !m_finishedEmitted) {
        QMutexLocker audioLocker(&m_audioFrameMutex);
        if (m_audioFrameQueue.isEmpty()) {
            m_finishedEmitted = true;
            emit playbackFinished();
        }
    }
    return;
}
```

### UI 高度自适应
- 播放列表面板展开时，`positionUIWindow` 自动增加 150px 高度
- 切换按钮蓝色高亮表示展开状态

---

## T4.6 截图

### 实现方案
- D3D11Renderer 新增 `capture()` 方法：
  1. `GetBuffer(0)` 获取 swap chain back buffer
  2. 创建 `D3D11_USAGE_STAGING` + `D3D11_CPU_ACCESS_READ` 纹理
  3. `CopyResource` 从 back buffer 到 staging texture
  4. `Map` 读取像素，按行拷贝（注意 RowPitch 对齐）
  5. 转换为 QImage，`Unmap` 后返回
- VideoWidget 新增 `capture()` 方法，加 `g_d3d11Mutex` 锁后调用渲染器截图
- 控制栏新增 📷 截图按钮，保存到 `D:\new\Player\picture\` 目录
- 保存结果用淡入淡出通知（无需手动关闭），2秒后自动消失

### 遇到的问题
- **截图色调偏蓝紫**：swap chain 格式是 `DXGI_FORMAT_R8G8B8A8_UNORM`（RGBA顺序），而 `QImage::Format_ARGB32` 在小端系统上是 BGRA 顺序 → 改用 `QImage::Format_RGBA8888` 直接对应

### 截图代码
```cpp
QImage D3D11Renderer::capture() {
    ComPtr<ID3D11Texture2D> backBuffer;
    m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuffer);
    D3D11_TEXTURE2D_DESC desc;
    backBuffer->GetDesc(&desc);
    // 创建staging texture
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = desc.Width;
    stagingDesc.Height = desc.Height;
    stagingDesc.Format = desc.Format;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stagingTex;
    m_device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
    m_context->CopyResource(stagingTex.Get(), backBuffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    m_context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    QImage image(desc.Width, desc.Height, QImage::Format_RGBA8888);
    for (UINT y = 0; y < desc.Height; y++)
        memcpy(image.scanLine(y), (uint8_t*)mapped.pData + y * mapped.RowPitch, desc.Width * 4);
    m_context->Unmap(stagingTex.Get(), 0);
    return image;
}
```

---

## T4.7 拖拽打开文件

### 实现方案
- MainWindowFilter 扩展：处理 `QEvent::DragEnter` 和 `QEvent::Drop`
- `DragEnter` 时检查 `mimeData()->hasUrls()`，接受 proposedAction
- `Drop` 时提取本地文件路径，调用回调添加到播放列表并播放第一个
- **必须在 window、central、videoWidget 三处都安装 eventFilter + setAcceptDrops(true)**

### 遇到的问题
- **拖拽无反应**：`QWidget::createWindowContainer` 创建的 videoWidget 容器拦截了拖拽事件 → 在 central 和 videoWidget 上也安装同一个 eventFilter 并启用拖拽接受

### 拖拽回调
```cpp
auto handleDroppedFiles = [&](const QStringList &files) {
    for (const QString &f : files)
        player->addToPlaylist(f);
    if (!files.isEmpty()) {
        int idx = player->playlist().indexOf(files.first());
        if (idx >= 0) {
            player->playIndex(idx);
            playBtn->setText("暂停");
        }
    }
};
```

---

## M4 阶段关键技术决策

1. **软解回退用动态纹理做中介**：D3D11_USAGE_DYNAMIC + CPU_WRITE，Map 写入后 CopySubresourceRegion 到中转纹理，与硬解路径统一
2. **字幕用独立悬浮窗**：避免被 D3D11 视频窗口裁剪/覆盖，支持任意位置拖动
3. **播放结束用三条件检测**：demux结束 + 视频队列空 + 音频队列空，确保音视频都播放完毕才切换
4. **截图用 Format_RGBA8888**：与 swap chain 的 R8G8B8A8 格式字节顺序一致，避免 R/B 通道颠倒
5. **拖拽三处安装过滤器**：createWindowContainer 容器会拦截事件，必须在所有层级都启用

## 已知限制
1. 个别关键帧间隔过长的视频，seek 到末尾几秒恢复较慢（视频本身特性）
2. 软解回退路径已实现但未充分端到端验证
3. 字幕样式（字体、颜色、边框）暂不支持自定义，使用默认样式
