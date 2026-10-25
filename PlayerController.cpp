#include "PlayerController.h"
#include "D3D11Renderer.h"
#include <QDebug>

// D3D11VA 设备上下文的锁回调（让FFmpeg解码时也获取g_d3d11Mutex，避免与渲染线程冲突）
static void d3d11va_lock(void *ctx) { static_cast<QMutex*>(ctx)->lock(); }
static void d3d11va_unlock(void *ctx) { static_cast<QMutex*>(ctx)->unlock(); }

PlayerController::PlayerController(QObject *parent)
    : QObject(parent)
{
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        qWarning() << "SDL_Init failed:" << SDL_GetError();
    }
    m_displayTimer = new QTimer(this);
    m_displayTimer->setTimerType(Qt::PreciseTimer);  // 高精度定时器，支持高帧率视频
    connect(m_displayTimer, &QTimer::timeout, this, &PlayerController::displayFrame);

    // seek 防抖定时器：80ms 内连续 seek 只执行最后一次（极致响应）
    m_seekTimer = new QTimer(this);
    m_seekTimer->setSingleShot(true);
    connect(m_seekTimer, &QTimer::timeout, this, &PlayerController::performSeek);
}

PlayerController::~PlayerController()
{
    stop();
    if (m_swUploadTexture) { m_swUploadTexture->Release(); m_swUploadTexture = nullptr; }
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_d3d11Context) { m_d3d11Context->Release(); m_d3d11Context = nullptr; }
    if (m_d3d11Device) { m_d3d11Device->Release(); m_d3d11Device = nullptr; }
    StagingTexturePool::instance().clear();
    SDL_Quit();
}

void PlayerController::setHardwareDecode(bool enable)
{
    m_hwDecodeRequested = enable;
}

void PlayerController::setVolume(float volume)
{
    m_volume = qBound(0.0f, volume, 2.0f);  // 限制0~2倍
}

bool PlayerController::openFile(const QString &filePath)
{
    stop();
    m_demuxFinished = false;
    m_finishedEmitted = false;
    m_currentFilePath = filePath;
    m_decodedFrames = 0;
    m_displayFrames = 0;

    if (avformat_open_input(&m_formatCtx, filePath.toUtf8().constData(), nullptr, nullptr) < 0) {
        // 失败必须把状态清干净：否则 m_formatCtx 残留非空，play() 会误判可播放并启动线程，
        // 解码线程随即在无效上下文上 avcodec_send_packet → 崩溃
        if (m_formatCtx) avformat_close_input(&m_formatCtx);
        m_videoStream = -1;
        m_audioStream = -1;
        emit errorOccurred("无法打开文件");
        return false;
    }

    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        avformat_close_input(&m_formatCtx);
        m_videoStream = -1;
        m_audioStream = -1;
        emit errorOccurred("无法找到流信息");
        return false;
    }

    for (unsigned int i = 0; i < m_formatCtx->nb_streams; i++) {
        if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && m_videoStream < 0) {
            m_videoStream = i;
        } else if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && m_audioStream < 0) {
            m_audioStream = i;
        }
    }

    if (m_videoStream < 0) {
        emit errorOccurred("未找到视频流");
        return false;
    }

    if (!openCodec(m_videoStream, m_videoCodecCtx)) {
        emit errorOccurred("无法打开视频解码器");
        return false;
    }

    if (m_audioStream >= 0) {
        if (!openCodec(m_audioStream, m_audioCodecCtx)) {
            emit errorOccurred("无法打开音频解码器");
            return false;
        }
        initAudio(m_audioCodecCtx);
    }

    if (m_formatCtx->duration != AV_NOPTS_VALUE) {
        m_durationMs = m_formatCtx->duration / 1000;
        emit durationChanged(m_durationMs);
    }

    return true;
}

bool PlayerController::openCodec(int streamIndex, AVCodecContext *&codecCtx)
{
    AVStream *stream = m_formatCtx->streams[streamIndex];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) return false;

    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) return false;

    if (avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0) return false;

    // 记录视频分辨率（T4.9 诊断）
    if (streamIndex == m_videoStream) {
        m_videoWidth = codecCtx->width;
        m_videoHeight = codecCtx->height;
    }

    // 视频解码器尝试D3D11VA硬件加速（设备只创建一次，切换视频时复用）
    if (streamIndex == m_videoStream && m_hwDecodeRequested) {
        for (int i = 0; ; i++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
            if (!config) break;
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                config->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
                // 设备复用：如果已有设备则直接用，否则创建
                if (!m_d3d11Device) {
                    D3D_FEATURE_LEVEL featureLevel;
                    HRESULT hr = D3D11CreateDevice(
                        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                        nullptr, 0, D3D11_SDK_VERSION,
                        &m_d3d11Device, &featureLevel, &m_d3d11Context);
                    if (FAILED(hr)) break;
                }
                // 创建设备上下文（每次新解码器都需要新的hwDeviceCtx，但复用同一个D3D11设备）
                m_hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
                if (!m_hwDeviceCtx) {
                    qWarning() << "av_hwdevice_ctx_alloc failed";
                    break;
                }
                AVD3D11VADeviceContext *d3d11vaCtx = (AVD3D11VADeviceContext*)av_mallocz(sizeof(AVD3D11VADeviceContext));
                if (!d3d11vaCtx) {
                    av_buffer_unref(&m_hwDeviceCtx);
                    break;
                }
                // 所有权约定（ffmpeg libavutil/hwcontext_d3d11va.c）：av_hwdevice_ctx_init 不会替你
                // 对用户提供的 device/device_context 做 AddRef，但释放 hwdevice_ctx 时
                // d3d11va_device_uninit 会无条件 Release 这两者。故交出前必须各 +1，
                // 否则 stop()/析构时双重 Release → 设备被提前销毁 → 0xC0000005。
                m_d3d11Device->AddRef();
                m_d3d11Context->AddRef();
                d3d11vaCtx->device = m_d3d11Device;
                d3d11vaCtx->device_context = m_d3d11Context;
                d3d11vaCtx->lock = d3d11va_lock;
                d3d11vaCtx->unlock = d3d11va_unlock;
                d3d11vaCtx->lock_ctx = &g_d3d11Mutex;
                AVHWDeviceContext *hwDevCtx = (AVHWDeviceContext*)m_hwDeviceCtx->data;
                hwDevCtx->hwctx = d3d11vaCtx;
                if (av_hwdevice_ctx_init(m_hwDeviceCtx) >= 0) {
                    codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
                    m_hwDecodeEnabled = true;
                    qDebug() << "D3D11VA hardware decoding enabled (reused device)";
                } else {
                    qWarning() << "av_hwdevice_ctx_init failed";
                    av_buffer_unref(&m_hwDeviceCtx);
                }
                break;
            }
        }
        if (!m_hwDecodeEnabled) {
            qDebug() << "D3D11VA unavailable, falling back to software decoding";
        }
    }

    // 视频流：软解时也需要D3D11设备（渲染+软解纹理上传）
    if (streamIndex == m_videoStream && !m_d3d11Device) {
        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION,
            &m_d3d11Device, &featureLevel, &m_d3d11Context);
        if (FAILED(hr)) {
            qWarning() << "D3D11CreateDevice failed (software decode path):" << hr;
        }
    }

    codecCtx->thread_count = 0;
    codecCtx->thread_type = FF_THREAD_FRAME;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) return false;

    return true;
}

// AVSampleFormat → SDL_AudioFormat 映射（packed 格式）
static SDL_AudioFormat avToSdlFormat(AVSampleFormat fmt)
{
    switch (fmt) {
    case AV_SAMPLE_FMT_U8:  return AUDIO_U8;
    case AV_SAMPLE_FMT_S16: return AUDIO_S16SYS;
    case AV_SAMPLE_FMT_S32: return AUDIO_S32SYS;
    case AV_SAMPLE_FMT_FLT: return AUDIO_F32SYS;
    default: return 0;  // 不支持
    }
}

void PlayerController::initAudio(AVCodecContext *audioCodecCtx)
{
    // 无损路径：目标采样率/声道数 = 源，目标格式 = 源格式的packed版本（planar→packed纯拷贝，无损失）
    int srcSampleRate = audioCodecCtx->sample_rate;
    int srcChannels = audioCodecCtx->ch_layout.nb_channels;
    AVSampleFormat srcFmt = audioCodecCtx->sample_fmt;
    AVSampleFormat packedFmt = av_get_packed_sample_fmt(srcFmt);
    if (packedFmt == AV_SAMPLE_FMT_NONE) packedFmt = AV_SAMPLE_FMT_FLT;

    m_outSampleRate = srcSampleRate;
    m_outChannels = srcChannels > 0 ? srcChannels : 2;
    m_outSampleFmt = packedFmt;
    m_outBytesPerSample = av_get_bytes_per_sample(packedFmt);
    if (m_outBytesPerSample <= 0) m_outBytesPerSample = 2;

    bool lossless = false;
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = m_outSampleRate;
    want.channels = m_outChannels;
    want.samples = 4096;
    want.callback = sdlAudioCallback;
    want.userdata = this;

    SDL_AudioFormat sdlFmt = avToSdlFormat(packedFmt);
    if (sdlFmt != 0) {
        want.format = sdlFmt;
        m_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (m_audioDevice != 0 &&
            have.freq == m_outSampleRate && have.channels == m_outChannels &&
            have.format == sdlFmt) {
            lossless = true;  // SDL 直通，无重采样无格式转换
            qDebug() << "Lossless audio:" << m_outSampleRate << "Hz,"
                     << m_outChannels << "ch," << av_get_sample_fmt_name(packedFmt);
        }
    }

    // 回退路径：SDL 不支持源格式时，重采样到 44100/S16
    if (!lossless) {
        if (m_audioDevice != 0) SDL_CloseAudioDevice(m_audioDevice);
        m_audioDevice = 0;
        m_outSampleRate = 44100;
        m_outChannels = 2;
        m_outSampleFmt = AV_SAMPLE_FMT_S16;
        SDL_zero(want);
        want.freq = m_outSampleRate;
        want.format = AUDIO_S16SYS;
        want.channels = m_outChannels;
        want.samples = 1024;
        want.callback = sdlAudioCallback;
        want.userdata = this;
        m_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (m_audioDevice == 0) {
            emit errorOccurred(QString("SDL音频设备打开失败: %1").arg(SDL_GetError()));
            return;
        }
        qDebug() << "Audio fallback: resample to 44100/S16 stereo";
    }

    // swr：无损路径仅做planar→packed，重采样路径做完整转换
    m_swrCtx = swr_alloc();
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, m_outChannels);
    av_opt_set_chlayout(m_swrCtx, "in_chlayout", &audioCodecCtx->ch_layout, 0);
    av_opt_set_chlayout(m_swrCtx, "out_chlayout", &outLayout, 0);
    av_opt_set_int(m_swrCtx, "in_sample_rate", srcSampleRate, 0);
    av_opt_set_int(m_swrCtx, "out_sample_rate", m_outSampleRate, 0);
    av_opt_set_sample_fmt(m_swrCtx, "in_sample_fmt", srcFmt, 0);
    av_opt_set_sample_fmt(m_swrCtx, "out_sample_fmt", m_outSampleFmt, 0);

    if (swr_init(m_swrCtx) < 0) {
        emit errorOccurred("音频重采样初始化失败");
        swr_free(&m_swrCtx);
    }
}

void PlayerController::sdlAudioCallback(void *userdata, Uint8 *stream, int len)
{
    PlayerController *self = static_cast<PlayerController*>(userdata);
    self->audioCallback(stream, len);
}

void PlayerController::audioCallback(Uint8 *stream, int len)
{
    QMutexLocker locker(&m_audioFrameMutex);
    SDL_memset(stream, 0, len);

    int remaining = len;
    while (remaining > 0 && !m_audioFrameQueue.isEmpty()) {
        QByteArray &chunk = m_audioFrameQueue.head();
        int copyLen = qMin(remaining, chunk.size());
        SDL_memcpy(stream + (len - remaining), chunk.constData(), copyLen);
        chunk.remove(0, copyLen);
        remaining -= copyLen;
        if (chunk.isEmpty()) {
            m_audioFrameQueue.dequeue();
        }
    }

    // 软件音量控制（PCM缩放，兼容所有SDL版本）
    int copied = len - remaining;
    if (m_volume != 1.0f && copied > 0) {
        int bytesPerSample = m_outBytesPerSample * m_outChannels;
        if (bytesPerSample > 0) {
            int totalSamples = copied / m_outBytesPerSample;  // 总采样点数（含声道）
            if (m_outSampleFmt == AV_SAMPLE_FMT_S16) {
                int16_t *ptr = reinterpret_cast<int16_t*>(stream);
                for (int i = 0; i < totalSamples; i++) {
                    int v = static_cast<int>(ptr[i] * m_volume);
                    ptr[i] = static_cast<int16_t>(qBound(-32768, v, 32767));
                }
            } else if (m_outSampleFmt == AV_SAMPLE_FMT_FLT) {
                float *ptr = reinterpret_cast<float*>(stream);
                for (int i = 0; i < totalSamples; i++) {
                    ptr[i] *= m_volume;
                }
            } else if (m_outSampleFmt == AV_SAMPLE_FMT_S32) {
                int32_t *ptr = reinterpret_cast<int32_t*>(stream);
                for (int i = 0; i < totalSamples; i++) {
                    long long v = static_cast<long long>(ptr[i] * m_volume);
                    ptr[i] = static_cast<int32_t>(qBound((long long)-2147483648, v, (long long)2147483647));
                }
            }
        }
    }

    // 音频时钟：已实际输出的样本数
    if (m_isRunning && !m_isPaused) {
        int bytesPerSample = m_outBytesPerSample * m_outChannels;
        if (bytesPerSample > 0) {
            m_audioPlayedSamples += copied / bytesPerSample;
        }
    }
}

// 停止所有工作线程：必须在各自条件变量对应的互斥量内改标志并唤醒。
// 否则 worker 可能刚判定 m_isRunning==true、尚未进入 wait()，此时 stop 置位并
// wakeAll（无人等待），worker 随后进入无超时 wait() → 永久阻塞，wait() 永不返回。
void PlayerController::signalWorkersToStop()
{
    {
        QMutexLocker locker(&m_pauseMutex);
        m_isRunning = false;
        m_isPaused = false;
        m_pauseCondition.wakeAll();
    }
    {
        QMutexLocker locker(&m_audioPacketMutex);
        m_audioPacketCondition.wakeAll();
    }
    {
        QMutexLocker locker(&m_videoPacketMutex);
        m_videoPacketCondition.wakeAll();
    }
    {
        QMutexLocker locker(&m_frameMutex);
        m_frameCondition.wakeAll();
    }
}

void PlayerController::startPlaybackThreads(bool resumePlayback)
{
    m_isRunning = true;

    m_audioDecodeThread = QThread::create([this]() { audioDecodeLoop(); });
    m_audioDecodeThread->start();

    m_videoDecodeThread = QThread::create([this]() { videoDecodeLoop(); });
    m_videoDecodeThread->start();

    m_demuxThread = QThread::create([this]() { demuxLoop(); });
    m_demuxThread->start();

    if (!resumePlayback) return;  // 暂停中 seek：只重建线程，等用户点播放再出声

    // 不做阻塞式预缓冲：旧实现在 GUI 线程 msleep 轮询最多 30*10ms + 20*10ms，
    // 每次 seek 都会冻结界面约 500ms。音频回调在队列为空时输出静音（stream 已 memset），
    // 首帧到达（约 20~40ms）后自然衔接。
    if (m_audioDevice != 0) SDL_PauseAudioDevice(m_audioDevice, 0);
    m_displayTimer->start(1);
}

void PlayerController::joinPlaybackThreads()
{
    if (m_demuxThread) { m_demuxThread->wait(); delete m_demuxThread; m_demuxThread = nullptr; }
    if (m_audioDecodeThread) { m_audioDecodeThread->wait(); delete m_audioDecodeThread; m_audioDecodeThread = nullptr; }
    if (m_videoDecodeThread) { m_videoDecodeThread->wait(); delete m_videoDecodeThread; m_videoDecodeThread = nullptr; }
}

void PlayerController::clearPacketQueues()
{
    {
        QMutexLocker locker(&m_audioPacketMutex);
        while (!m_audioPacketQueue.isEmpty()) {
            AVPacket *pkt = m_audioPacketQueue.dequeue();
            av_packet_free(&pkt);
        }
    }
    {
        QMutexLocker locker(&m_videoPacketMutex);
        while (!m_videoPacketQueue.isEmpty()) {
            AVPacket *pkt = m_videoPacketQueue.dequeue();
            av_packet_free(&pkt);
        }
    }
}

void PlayerController::clearFrameQueues()
{
    {
        QMutexLocker locker(&m_audioFrameMutex);
        m_audioFrameQueue.clear();
    }
    {
        QMutexLocker locker(&m_frameMutex);
        while (!m_videoFrameQueue.isEmpty()) {
            auto pair = m_videoFrameQueue.dequeue();
            if (pair.first.texture) StagingTexturePool::instance().release(pair.first.texture);
        }
    }
}

void PlayerController::stopPlaybackThreads()
{
    signalWorkersToStop();
    m_displayTimer->stop();
    if (m_audioDevice != 0) {
        SDL_PauseAudioDevice(m_audioDevice, 1);
        // 丢弃 SDL 设备队列里残留的 PCM：否则 seek/停止后旧音频继续播放，音画错位
        SDL_ClearQueuedAudio(m_audioDevice);
    }
    joinPlaybackThreads();
}

void PlayerController::play()
{
    if (!m_formatCtx || m_videoStream < 0) {
        emit errorOccurred("请先打开视频文件");
        return;
    }

    if (m_isRunning) {
        m_isPaused = false;
        m_pauseCondition.wakeAll();
        m_audioPacketCondition.wakeAll();
        m_videoPacketCondition.wakeAll();
        // 恢复播放时重置插值基准，避免暂停期间时间被算入
        m_lastClockSamples = m_audioPlayedSamples.load();
        m_lastClockTime = QDateTime::currentMSecsSinceEpoch();
        if (m_audioDevice != 0) SDL_PauseAudioDevice(m_audioDevice, 0);
        m_displayTimer->start(1);
        return;
    }

    m_lastClockSamples = 0;
    m_lastClockTime = QDateTime::currentMSecsSinceEpoch();
    m_seekTime = 0;

    startPlaybackThreads();
}

void PlayerController::pause()
{
    m_isPaused = true;
    if (m_audioDevice != 0) SDL_PauseAudioDevice(m_audioDevice, 1);
    m_displayTimer->stop();
}

void PlayerController::stop()
{
    signalWorkersToStop();
    m_displayTimer->stop();

    joinPlaybackThreads();

    if (m_audioDevice != 0) {
        SDL_CloseAudioDevice(m_audioDevice);
        m_audioDevice = 0;
    }

    if (m_swrCtx) { swr_free(&m_swrCtx); }
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }  // 切换视频时源格式可能变化
    if (m_hwDeviceCtx) { av_buffer_unref(&m_hwDeviceCtx); m_hwDecodeEnabled = false; }
    // 注意：不释放 m_d3d11Device/m_d3d11Context，切换视频时复用
    // 注意：不调用 StagingTexturePool::clear()，设备不变纹理可复用
    if (m_videoCodecCtx) { avcodec_free_context(&m_videoCodecCtx); }
    if (m_audioCodecCtx) { avcodec_free_context(&m_audioCodecCtx); }
    if (m_formatCtx) { avformat_close_input(&m_formatCtx); }

    m_videoStream = -1;
    m_audioStream = -1;
    clearFrameQueues();
    m_audioPlayedSamples = 0;  // 音频时钟清零，重新播放从0开始
    m_lastClockSamples = 0;
    m_lastClockTime = 0;
    // 字幕状态重置（不清除已加载的字幕文件，只清除当前显示）
    if (!m_currentSubtitleText.isEmpty()) {
        m_currentSubtitleText.clear();
        emit subtitleChanged(QString());
    }

    clearPacketQueues();
}

bool PlayerController::isPlaying() const
{
    return m_isRunning && !m_isPaused;
}

void PlayerController::seek(qint64 positionMs)
{
    if (!m_formatCtx || m_videoStream < 0) return;

    // seek 防抖：80ms 内连续 seek 只执行最后一次（极致响应）
    m_pendingSeekPos = positionMs;
    m_seekTimer->start(80);
}

void PlayerController::performSeek()
{
    if (!m_formatCtx || m_videoStream < 0 || m_pendingSeekPos < 0) return;

    qint64 positionMs = m_pendingSeekPos;
    m_pendingSeekPos = -1;

    emit seekStarted();  // 通知渲染端清理pending纹理

    bool wasRunning = m_isRunning;
    bool wasPaused = m_isPaused;

    // 停止所有解码线程（确保 av_seek_frame 时无线程操作 format context/解码器）
    stopPlaybackThreads();

    // 计算目标时间戳
    double targetSec = positionMs / 1000.0;
    AVStream *videoStream = m_formatCtx->streams[m_videoStream];
    int64_t targetTs = (int64_t)(targetSec / av_q2d(videoStream->time_base));

    // seek 到最近关键帧（向后找，保证从关键帧开始解码）
    av_seek_frame(m_formatCtx, m_videoStream, targetTs, AVSEEK_FLAG_BACKWARD);

    // 清空四个队列（包 + 帧）
    clearPacketQueues();
    // 清空AudioTap缓冲区（seek后时间线不连续）
    if (m_audioTapEnabled) {
        QMutexLocker tapLocker(&m_audioTapMutex);
        m_audioTapBuffer.clear();
        if (m_audioTapSwr) {
            swr_free(&m_audioTapSwr);
            m_audioTapSwr = nullptr;
        }
    }
    clearFrameQueues();

    // 刷新解码器缓冲区（丢弃 seek 前的残留帧）
    if (m_videoCodecCtx) avcodec_flush_buffers(m_videoCodecCtx);
    if (m_audioCodecCtx) avcodec_flush_buffers(m_audioCodecCtx);

    // 重置demux结束标志（seek后重新读包）
    m_demuxFinished = false;

    // 重置音频时钟，设置 seek 偏移
    m_seekTime = targetSec;
    m_audioPlayedSamples = 0;
    m_lastClockSamples = 0;
    m_lastClockTime = QDateTime::currentMSecsSinceEpoch();

    // 重新启动线程（如果之前在运行）
    if (wasRunning) {
        m_isPaused = wasPaused;
        startPlaybackThreads(!wasPaused);
    }
}

// ==================== 播放列表 ====================

void PlayerController::addToPlaylist(const QString &filePath)
{
    if (!m_playlist.contains(filePath)) {
        m_playlist.append(filePath);
        emit playlistChanged();
    }
}

void PlayerController::removeFromPlaylist(int index)
{
    if (index >= 0 && index < m_playlist.size()) {
        m_playlist.removeAt(index);
        if (m_currentIndex == index) {
            stop();
            m_currentIndex = -1;
            emit currentIndexChanged(-1);
        } else if (m_currentIndex > index) {
            m_currentIndex--;
        }
        emit playlistChanged();
    }
}

void PlayerController::playIndex(int index)
{
    if (index < 0 || index >= m_playlist.size()) return;
    m_currentIndex = index;
    emit currentIndexChanged(index);
    if (openFile(m_playlist[index])) {
        play();
    }
}

void PlayerController::playNext()
{
    if (m_currentIndex + 1 < m_playlist.size()) {
        playIndex(m_currentIndex + 1);
    }
}

// === 软解帧上传到D3D11纹理 ===
bool PlayerController::ensureSwUploadTexture(int width, int height)
{
    if (m_swUploadTexture && m_swUploadW == width && m_swUploadH == height) return true;
    if (m_swUploadTexture) { m_swUploadTexture->Release(); m_swUploadTexture = nullptr; }
    if (!m_d3d11Device) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = 0;  // 动态纹理不能作为SRV，仅作上传中介
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hr = m_d3d11Device->CreateTexture2D(&desc, nullptr, &m_swUploadTexture);
    if (FAILED(hr)) {
        qWarning() << "[SW] Create dynamic upload texture failed:" << hr;
        return false;
    }
    m_swUploadW = width;
    m_swUploadH = height;
    qDebug() << "[SW] Dynamic upload texture created:" << width << "x" << height;
    return true;
}

bool PlayerController::uploadSoftwareFrame(AVFrame *frame, ID3D11Texture2D *&outStagingTex)
{
    outStagingTex = nullptr;
    if (!m_d3d11Device || !m_d3d11Context) return false;
    int w = frame->width, h = frame->height;

    // 1. sws_scale 转换为 NV12（semi-planar: Y + UV交错）
    if (!m_swsCtx) {
        m_swsCtx = sws_getContext(w, h, (AVPixelFormat)frame->format,
            w, h, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_swsCtx) { qWarning() << "[SW] sws_getContext failed"; return false; }
    }
    // 分配NV12临时缓冲区
    int nv12YStride = w;
    int nv12UVStride = w;
    QByteArray nv12Buf(w * h * 3 / 2, 0);
    uint8_t *dstY = (uint8_t*)nv12Buf.data();
    uint8_t *dstUV = dstY + w * h;
    uint8_t *dstData[2] = { dstY, dstUV };
    int dstLinesize[2] = { nv12YStride, nv12UVStride };
    sws_scale(m_swsCtx, frame->data, frame->linesize, 0, h, dstData, dstLinesize);

    // 2. 确保动态上传纹理存在
    if (!ensureSwUploadTexture(w, h)) return false;

    // 3. Map动态纹理，写入NV12数据
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_d3d11Context->Map(m_swUploadTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) { qWarning() << "[SW] Map failed:" << hr; return false; }
    // Y平面
    uint8_t *dst = (uint8_t*)mapped.pData;
    for (int y = 0; y < h; y++) {
        memcpy(dst + y * mapped.RowPitch, dstY + y * nv12YStride, w);
    }
    // UV平面（NV12的UV在Y之后，高度h/2）
    uint8_t *uvDst = dst + mapped.RowPitch * h;
    for (int y = 0; y < h / 2; y++) {
        memcpy(uvDst + y * mapped.RowPitch, dstUV + y * nv12UVStride, w);
    }
    m_d3d11Context->Unmap(m_swUploadTexture, 0);

    // 4. Copy到中转纹理（DEFAULT usage，可被着色器采样）
    ID3D11Texture2D *stagingTex = StagingTexturePool::instance().acquire(m_d3d11Device, w, h);
    if (!stagingTex) return false;
    {
        QMutexLocker d3dLocker(&g_d3d11Mutex);
        m_d3d11Context->CopySubresourceRegion(stagingTex, 0, 0, 0, 0, m_swUploadTexture, 0, nullptr);
    }
    outStagingTex = stagingTex;
    return true;
}

// 读包线程：只负责读包和分发，不做解码
void PlayerController::demuxLoop()
{
    AVPacket *packet = av_packet_alloc();

    while (m_isRunning) {
        if (m_isPaused) {
            QMutexLocker locker(&m_pauseMutex);
            m_pauseCondition.wait(&m_pauseMutex);
            continue;
        }

        if (av_read_frame(m_formatCtx, packet) < 0) {
            break;
        }

        if (packet->stream_index == m_videoStream) {
            AVPacket *pktCopy = av_packet_clone(packet);
            QMutexLocker locker(&m_videoPacketMutex);
            if (m_videoPacketQueue.size() >= 400) {
                // 队列满：非关键帧丢弃，关键帧必须保留等待
                if (!(pktCopy->flags & AV_PKT_FLAG_KEY)) {
                    av_packet_free(&pktCopy);
                } else {
                    while (m_videoPacketQueue.size() >= 400 && m_isRunning) {
                        m_videoPacketCondition.wait(&m_videoPacketMutex);
                    }
                    if (m_isRunning) {
                        m_videoPacketQueue.enqueue(pktCopy);
                        m_videoPacketCondition.wakeOne();
                    } else {
                        av_packet_free(&pktCopy);
                    }
                }
            } else {
                m_videoPacketQueue.enqueue(pktCopy);
                m_videoPacketCondition.wakeOne();
            }
        } else if (packet->stream_index == m_audioStream) {
            AVPacket *pktCopy = av_packet_clone(packet);
            QMutexLocker locker(&m_audioPacketMutex);
            while (m_audioPacketQueue.size() >= 100 && m_isRunning) {
                m_audioPacketCondition.wait(&m_audioPacketMutex);
            }
            if (m_isRunning) {
                m_audioPacketQueue.enqueue(pktCopy);
                m_audioPacketCondition.wakeOne();
            } else {
                av_packet_free(&pktCopy);
            }
        }

        av_packet_unref(packet);
    }

    m_demuxFinished = true;
    av_packet_free(&packet);

    // 给音频和视频解码线程发送flush标记（nullptr包），让解码器输出所有缓冲帧
    {
        QMutexLocker locker(&m_audioPacketMutex);
        m_audioPacketQueue.enqueue(nullptr);
        m_audioPacketCondition.wakeOne();
    }
    {
        QMutexLocker locker(&m_videoPacketMutex);
        m_videoPacketQueue.enqueue(nullptr);
        m_videoPacketCondition.wakeOne();
    }
}

// 音频解码线程
void PlayerController::audioDecodeLoop()
{
    AVFrame *frame = av_frame_alloc();

    while (m_isRunning) {
        if (m_isPaused) {
            QMutexLocker locker(&m_pauseMutex);
            m_pauseCondition.wait(&m_pauseMutex);
            continue;
        }

        AVPacket *packet = nullptr;
        {
            QMutexLocker locker(&m_audioPacketMutex);
            while (m_audioPacketQueue.isEmpty() && m_isRunning) {
                m_audioPacketCondition.wait(&m_audioPacketMutex);
            }
            if (!m_audioPacketQueue.isEmpty()) {
                packet = m_audioPacketQueue.dequeue();
                m_audioPacketCondition.wakeOne();
            }
        }

        // 音频帧处理lambda（swr转换+入队）
        auto processAudioFrame = [&]() {
            // seek后跳过远早于目标位置的音频帧，保持音画同步
            if (m_audioStream >= 0) {
                double audioPts = frame->best_effort_timestamp * av_q2d(m_formatCtx->streams[m_audioStream]->time_base);
                if (audioPts >= 0 && audioPts < m_seekTime - 0.1) {
                    return;  // 丢弃早帧，不做swr转换
                }
            }
            while (m_audioFrameQueue.size() > 50 && m_isRunning) {
                QThread::msleep(5);
            }
            uint8_t *outBuffer = nullptr;
            int outSamples = swr_get_out_samples(m_swrCtx, frame->nb_samples);
            if (outSamples > 0) {
                int allocRet = av_samples_alloc(&outBuffer, nullptr, m_outChannels, outSamples, m_outSampleFmt, 0);
                if (allocRet >= 0) {
                    int converted = swr_convert(m_swrCtx, &outBuffer, outSamples,
                        (const uint8_t**)frame->data, frame->nb_samples);
                    if (converted > 0) {
                        int bufferSize = av_samples_get_buffer_size(nullptr, m_outChannels, converted, m_outSampleFmt, 1);
                        if (bufferSize > 0) {
                            QByteArray chunk(reinterpret_cast<const char*>(outBuffer), bufferSize);
                            QMutexLocker locker(&m_audioFrameMutex);
                            m_audioFrameQueue.enqueue(chunk);
                        }
                    }
                }
                av_freep(&outBuffer);
            }

            // AudioTap：将原始帧转换为16kHz mono float32并追加到缓冲区
            if (m_audioTapEnabled) {
                QMutexLocker tapLocker(&m_audioTapMutex);
                if (!m_audioTapSwr) {
                    m_audioTapSwr = swr_alloc();
                    AVChannelLayout monoLayout = AV_CHANNEL_LAYOUT_MONO;
                    av_opt_set_chlayout(m_audioTapSwr, "in_chlayout", &frame->ch_layout, 0);
                    av_opt_set_chlayout(m_audioTapSwr, "out_chlayout", &monoLayout, 0);
                    av_opt_set_int(m_audioTapSwr, "in_sample_rate", frame->sample_rate, 0);
                    av_opt_set_int(m_audioTapSwr, "out_sample_rate", 16000, 0);
                    av_opt_set_sample_fmt(m_audioTapSwr, "in_sample_fmt", static_cast<AVSampleFormat>(frame->format), 0);
                    av_opt_set_sample_fmt(m_audioTapSwr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
                    if (swr_init(m_audioTapSwr) < 0) {
                        qWarning() << "[AudioTap] swr_init失败";
                        swr_free(&m_audioTapSwr);
                        m_audioTapSwr = nullptr;
                    }
                }
                if (m_audioTapSwr) {
                    int tapOutSamples = swr_get_out_samples(m_audioTapSwr, frame->nb_samples);
                    if (tapOutSamples > 0) {
                        float *tapBuffer = nullptr;
                        int tapAlloc = av_samples_alloc((uint8_t**)&tapBuffer, nullptr, 1, tapOutSamples, AV_SAMPLE_FMT_FLT, 0);
                        if (tapAlloc >= 0) {
                            int tapConverted = swr_convert(m_audioTapSwr, (uint8_t**)&tapBuffer, tapOutSamples,
                                (const uint8_t**)frame->data, frame->nb_samples);
                            if (tapConverted > 0) {
                                int oldSize = m_audioTapBuffer.size();
                                m_audioTapBuffer.resize(oldSize + tapConverted);
                                memcpy(m_audioTapBuffer.data() + oldSize, tapBuffer, tapConverted * sizeof(float));
                            }
                            av_freep(&tapBuffer);
                        }
                    }
                }
            }
        };

        if (!packet) {
            // flush标记：让解码器输出所有缓冲帧
            avcodec_send_packet(m_audioCodecCtx, nullptr);
            while (avcodec_receive_frame(m_audioCodecCtx, frame) == 0) {
                processAudioFrame();
            }
            continue;
        }

        // 正确处理send_packet返回值
        int ret = avcodec_send_packet(m_audioCodecCtx, packet);
        if (ret == AVERROR(EAGAIN)) {
            while (avcodec_receive_frame(m_audioCodecCtx, frame) == 0) {
                processAudioFrame();
            }
            ret = avcodec_send_packet(m_audioCodecCtx, packet);
        }
        if (ret >= 0) {
            while (avcodec_receive_frame(m_audioCodecCtx, frame) == 0) {
                processAudioFrame();
            }
        }

        av_packet_free(&packet);
    }

    av_frame_free(&frame);
}

// 视频解码线程 — D3D11VA零拷贝：解码帧直接是GPU纹理，不入CPU
void PlayerController::videoDecodeLoop()
{
    AVFrame *frame = av_frame_alloc();

    while (m_isRunning) {
        if (m_isPaused) {
            QMutexLocker locker(&m_pauseMutex);
            m_pauseCondition.wait(&m_pauseMutex);
            continue;
        }

        AVPacket *packet = nullptr;
        {
            QMutexLocker locker(&m_videoPacketMutex);
            while (m_videoPacketQueue.isEmpty() && m_isRunning) {
                m_videoPacketCondition.wait(&m_videoPacketMutex);
            }
            if (!m_videoPacketQueue.isEmpty()) {
                packet = m_videoPacketQueue.dequeue();
                m_videoPacketCondition.wakeOne();
            }
        }

        if (!packet) {
            // flush标记：让解码器输出所有缓冲帧
            avcodec_send_packet(m_videoCodecCtx, nullptr);
            while (avcodec_receive_frame(m_videoCodecCtx, frame) == 0) {
                double pts = frame->best_effort_timestamp * av_q2d(m_formatCtx->streams[m_videoStream]->time_base);
                if (pts < 0 || pts != pts) { av_frame_unref(frame); continue; }
                // seek后快速跳过远早于目标位置的帧，不做纹理拷贝加速解码
                if (pts < m_seekTime - 0.1) { av_frame_unref(frame); continue; }
                ID3D11Texture2D *stagingTex = nullptr;
                if (frame->format == AV_PIX_FMT_D3D11) {
                    ID3D11Texture2D *srcTex = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
                    UINT subres = (UINT)(uintptr_t)frame->data[1];
                    if (!srcTex) { av_frame_unref(frame); continue; }
                    D3D11_TEXTURE2D_DESC srcDesc;
                    srcTex->GetDesc(&srcDesc);
                    if (srcDesc.Format != DXGI_FORMAT_NV12) { av_frame_unref(frame); continue; }
                    stagingTex = StagingTexturePool::instance().acquire(m_d3d11Device, frame->width, frame->height);
                    if (!stagingTex) { av_frame_unref(frame); continue; }
                    {
                        QMutexLocker d3dLocker(&g_d3d11Mutex);
                        m_d3d11Context->CopySubresourceRegion(stagingTex, 0, 0, 0, 0, srcTex, subres, nullptr);
                    }
                } else {
                    if (!uploadSoftwareFrame(frame, stagingTex)) { av_frame_unref(frame); continue; }
                }
                VideoFrameData vf = { stagingTex, 0, frame->width, frame->height };
                QMutexLocker locker2(&m_frameMutex);
                m_videoFrameQueue.enqueue(qMakePair(vf, pts));
                m_decodedFrames++;
                av_frame_unref(frame);
            }
            continue;
        }

        int ret = avcodec_send_packet(m_videoCodecCtx, packet);
        if (ret == AVERROR(EAGAIN)) {
            while (avcodec_receive_frame(m_videoCodecCtx, frame) == 0) {
                double pts = frame->best_effort_timestamp * av_q2d(m_formatCtx->streams[m_videoStream]->time_base);
                if (pts < 0 || pts != pts) { av_frame_unref(frame); continue; }
                // seek后快速跳过远早于目标位置的帧，不做纹理拷贝加速解码
                if (pts < m_seekTime - 0.1) { av_frame_unref(frame); continue; }
                ID3D11Texture2D *stagingTex = nullptr;
                if (frame->format == AV_PIX_FMT_D3D11) {
                    // 硬解路径：拷贝解码纹理到中转纹理
                    ID3D11Texture2D *srcTex = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
                    UINT subres = (UINT)(uintptr_t)frame->data[1];
                    if (!srcTex) continue;
                    D3D11_TEXTURE2D_DESC srcDesc;
                    srcTex->GetDesc(&srcDesc);
                    if (srcDesc.Format != DXGI_FORMAT_NV12) continue;
                    stagingTex = StagingTexturePool::instance().acquire(m_d3d11Device, frame->width, frame->height);
                    if (!stagingTex) continue;
                    {
                        QMutexLocker d3dLocker(&g_d3d11Mutex);
                        m_d3d11Context->CopySubresourceRegion(stagingTex, 0, 0, 0, 0, srcTex, subres, nullptr);
                    }
                } else {
                    // 软解路径：YUV→NV12→动态纹理→中转纹理
                    if (!uploadSoftwareFrame(frame, stagingTex)) continue;
                }
                VideoFrameData vf = { stagingTex, 0, frame->width, frame->height };
                QMutexLocker locker2(&m_frameMutex);
                while (m_videoFrameQueue.size() >= 10 && m_isRunning) {
                    m_frameCondition.wait(&m_frameMutex, 20);
                }
                if (m_isRunning) {
                    m_videoFrameQueue.enqueue(qMakePair(vf, pts));
                    m_decodedFrames++;
                } else {
                    StagingTexturePool::instance().release(stagingTex);
                }
            }
            ret = avcodec_send_packet(m_videoCodecCtx, packet);
        }
        if (ret >= 0) {
            while (avcodec_receive_frame(m_videoCodecCtx, frame) == 0) {
                double pts = frame->best_effort_timestamp * av_q2d(m_formatCtx->streams[m_videoStream]->time_base);
                if (pts < 0 || pts != pts) { av_frame_unref(frame); continue; }
                // seek后快速跳过远早于目标位置的帧，不做纹理拷贝加速解码
                if (pts < m_seekTime - 0.1) { av_frame_unref(frame); continue; }
                ID3D11Texture2D *stagingTex = nullptr;
                if (frame->format == AV_PIX_FMT_D3D11) {
                    // 硬解路径
                    ID3D11Texture2D *srcTex = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
                    UINT subres = (UINT)(uintptr_t)frame->data[1];
                    if (!srcTex) continue;
                    D3D11_TEXTURE2D_DESC srcDesc;
                    srcTex->GetDesc(&srcDesc);
                    if (srcDesc.Format != DXGI_FORMAT_NV12) continue;
                    stagingTex = StagingTexturePool::instance().acquire(m_d3d11Device, frame->width, frame->height);
                    if (!stagingTex) continue;
                    {
                        QMutexLocker d3dLocker(&g_d3d11Mutex);
                        m_d3d11Context->CopySubresourceRegion(stagingTex, 0, 0, 0, 0, srcTex, subres, nullptr);
                    }
                } else {
                    // 软解路径
                    if (!uploadSoftwareFrame(frame, stagingTex)) continue;
                }
                VideoFrameData vf = { stagingTex, 0, frame->width, frame->height };
                QMutexLocker locker2(&m_frameMutex);
                while (m_videoFrameQueue.size() >= 10 && m_isRunning) {
                    m_frameCondition.wait(&m_frameMutex, 20);
                }
                if (m_isRunning) {
                    m_videoFrameQueue.enqueue(qMakePair(vf, pts));
                    m_decodedFrames++;
                } else {
                    StagingTexturePool::instance().release(stagingTex);
                }
            }
        }

        av_packet_free(&packet);
    }

    av_frame_free(&frame);
}

void PlayerController::displayFrame()
{
    QMutexLocker locker(&m_frameMutex);

    if (m_videoFrameQueue.isEmpty()) {
        // 播放结束检测：demux结束 + 视频队列空 + 音频队列空
        if (m_demuxFinished && !m_finishedEmitted) {
            QMutexLocker audioLocker(&m_audioFrameMutex);
            if (m_audioFrameQueue.isEmpty()) {
                m_finishedEmitted = true;
                audioLocker.unlock();
                locker.unlock();
                emit playbackFinished();
                return;
            }
        }
        return;
    }

    // 文件末尾保护：demux结束且队列空时保持上一帧画面（flush后所有帧已解码，全部显示）
    if (m_demuxFinished && m_videoFrameQueue.isEmpty()) {
        return;  // 保持上一帧画面
    }

    // 音频时钟插值：SDL回调间隔较长(约23ms)，用系统时钟在两次回调之间平滑推进
    qint64 curSamples = m_audioPlayedSamples.load();
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (curSamples != m_lastClockSamples) {
        m_lastClockSamples = curSamples;
        m_lastClockTime = now;
    }
    double playTime;
    qint64 clockGap = now - m_lastClockTime;
    if (curSamples == 0 || clockGap > 100) {
        // 音频未开始播放 或 音频样本超过100ms未更新（音频结束/卡顿），停止系统时钟插值
        playTime = m_seekTime + m_lastClockSamples / (double)m_outSampleRate;
    } else {
        playTime = m_seekTime + m_lastClockSamples / (double)m_outSampleRate
                        + clockGap / 1000.0;
    }

    VideoFrameData frameToShow;
    double shownPts = 0;
    int dequeued = 0;
    while (!m_videoFrameQueue.isEmpty()) {
        QPair<VideoFrameData, double> &front = m_videoFrameQueue.head();
        if (front.second <= playTime) {
            if (frameToShow.texture) StagingTexturePool::instance().release(frameToShow.texture);
            frameToShow = front.first;
            shownPts = front.second;
            m_videoFrameQueue.dequeue();
            dequeued++;
        } else {
            break;
        }
    }
    if (dequeued > 0) {
        m_frameCondition.wakeOne();
    }

    if (frameToShow.texture) {
        m_displayFrames++;
        emit frameReady(frameToShow.texture, frameToShow.subresource, frameToShow.width, frameToShow.height);
        emit positionChanged(static_cast<qint64>(shownPts * 1000));
    }

    // 字幕同步：基于音频时钟获取当前应显示的字幕（优先显示译文）
    if (m_subtitleParser.isLoaded()) {
        SubtitleCue cue = m_subtitleParser.getCueObjectAtTime(playTime);
        QString subText = cue.translatedText.isEmpty() ? cue.text : cue.translatedText;
        if (subText != m_currentSubtitleText) {
            m_currentSubtitleText = subText;
            emit subtitleChanged(subText);
        }
    }

    // 每秒诊断：解码帧率/显示帧率/队列深度/音视频差距
    static int lastSec = -1;
    static int lastDec = 0;
    static int lastDisp = 0;
    int sec = static_cast<int>(playTime);
    if (sec != lastSec) {
        int dec = m_decodedFrames.load();
        int disp = m_displayFrames.load();
        m_lastDecodeFps = dec - lastDec;
        m_lastDisplayFps = disp - lastDisp;
        double headPts = m_videoFrameQueue.isEmpty() ? -1 : m_videoFrameQueue.head().second;
        qDebug() << "[DIAG] t=" << playTime
                 << "decodeFps=" << m_lastDecodeFps
                 << "displayFps=" << m_lastDisplayFps
                 << "frameQueue=" << m_videoFrameQueue.size()
                 << "headPts=" << headPts;
        lastSec = sec;
        lastDec = dec;
        lastDisp = disp;
    }
}

// ==================== 字幕 ====================

bool PlayerController::loadSubtitle(const QString &filePath)
{
    if (m_subtitleParser.loadFile(filePath)) {
        m_currentSubtitleText.clear();
        qDebug() << "[Subtitle] loaded:" << filePath << "cues:" << m_subtitleParser.cueCount();
        return true;
    }
    qWarning() << "[Subtitle] failed to load:" << filePath;
    return false;
}

void PlayerController::clearSubtitle()
{
    m_subtitleParser.clear();
    if (!m_currentSubtitleText.isEmpty()) {
        m_currentSubtitleText.clear();
        emit subtitleChanged(QString());
    }
}

bool PlayerController::hasSubtitle() const
{
    return m_subtitleParser.isLoaded();
}

void PlayerController::setAISubtitles(const QList<SubtitleCue> &cues)
{
    m_subtitleParser.setCues(cues);
    qDebug() << "[PlayerController] AI字幕已设置，条目数:" << cues.size();
}

QList<SubtitleCue> PlayerController::aiSubtitles() const
{
    return m_subtitleParser.cues();
}

// ===== AudioTap（T4.8）：录制16kHz mono float32音频供whisper识别 =====

void PlayerController::startAudioTap()
{
    QMutexLocker locker(&m_audioTapMutex);
    m_audioTapBuffer.clear();
    m_audioTapEnabled = true;
    qDebug() << "[AudioTap] 开始录制";
}

void PlayerController::stopAudioTap()
{
    QMutexLocker locker(&m_audioTapMutex);
    m_audioTapEnabled = false;
    if (m_audioTapSwr) {
        swr_free(&m_audioTapSwr);
        m_audioTapSwr = nullptr;
    }
    qDebug() << "[AudioTap] 停止录制，样本数:" << m_audioTapBuffer.size();
}

QVector<float> PlayerController::getAudioTapSamples() const
{
    QMutexLocker locker(&m_audioTapMutex);
    return m_audioTapBuffer;
}

// ==================== T4.9 诊断状态 ====================

PlayerController::DiagnosticStats PlayerController::diagnosticStats() const
{
    DiagnosticStats stats;
    stats.currentFile = m_currentFilePath;
    stats.durationMs = m_durationMs;
    stats.decodeFps = m_lastDecodeFps;
    stats.displayFps = m_lastDisplayFps;
    stats.hwDecodeEnabled = m_hwDecodeEnabled;
    stats.videoWidth = m_videoWidth;
    stats.videoHeight = m_videoHeight;

    // 加锁访问队列，避免多线程竞争
    {
        QMutexLocker locker(&m_frameMutex);
        stats.videoFrameQueueSize = m_videoFrameQueue.size();
        stats.videoClock = m_videoFrameQueue.isEmpty() ? -1 : m_videoFrameQueue.head().second;
    }
    {
        QMutexLocker locker(&m_audioFrameMutex);
        stats.audioFrameQueueSize = m_audioFrameQueue.size();
    }
    {
        QMutexLocker locker(&m_videoPacketMutex);
        stats.videoPacketQueueSize = m_videoPacketQueue.size();
    }
    {
        QMutexLocker locker(&m_audioPacketMutex);
        stats.audioPacketQueueSize = m_audioPacketQueue.size();
    }

    // 计算当前播放位置和音视频时钟
    qint64 curSamples = m_audioPlayedSamples.load();
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 clockGap = now - m_lastClockTime;
    if (curSamples == 0 || clockGap > 100) {
        stats.audioClock = m_seekTime + m_lastClockSamples / (double)m_outSampleRate;
    } else {
        stats.audioClock = m_seekTime + m_lastClockSamples / (double)m_outSampleRate + clockGap / 1000.0;
    }
    stats.positionMs = static_cast<qint64>(stats.audioClock * 1000);

    return stats;
}
