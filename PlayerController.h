#ifndef PLAYERCONTROLLER_H
#define PLAYERCONTROLLER_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QImage>
#include <QQueue>
#include <QTimer>
#include <QPair>
#include <QDateTime>
#include <atomic>

#include "AudioRingBuffer.h"
#include "SubtitleParser.h"
#include <SDL.h>
#include <d3d11.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

// D3D11 纹理帧（零拷贝，解码线程AddRef，渲染线程Release）
struct VideoFrameData {
    ID3D11Texture2D *texture = nullptr;
    UINT subresource = 0;
    int width = 0;
    int height = 0;
};

class PlayerController : public QObject
{
    Q_OBJECT
public:
    explicit PlayerController(QObject *parent = nullptr);
    ~PlayerController();

    bool openFile(const QString &filePath);
    void play();
    void pause();
    void stop();
    void seek(qint64 positionMs);  // 跳转到指定位置（毫秒）
    bool isPlaying() const;

    // 播放列表
    void addToPlaylist(const QString &filePath);
    void removeFromPlaylist(int index);
    void playIndex(int index);
    void playNext();
    QStringList playlist() const { return m_playlist; }
    int currentIndex() const { return m_currentIndex; }

    // 策略2：质量/性能配置（播放前设置）
    void setHardwareDecode(bool enable);       // true=尝试D3D11VA硬件解码，false=强制软件解码
    void setVolume(float volume);              // 0.0=静音，1.0=原音量，>1.0=放大

    // 字幕
    bool loadSubtitle(const QString &filePath);  // 加载外部字幕文件（SRT/ASS）
    void clearSubtitle();
    bool hasSubtitle() const;
    void setAISubtitles(const QList<SubtitleCue> &cues);  // 设置AI识别结果为字幕
    QList<SubtitleCue> aiSubtitles() const;  // 获取当前AI字幕（含翻译）

    // AudioTap（T4.8）：录制音频供离线AI识别
    void startAudioTap();           // 开始录制（清空缓冲区）
    void stopAudioTap();            // 停止录制
    bool isAudioTapEnabled() const { return m_audioTapEnabled; }
    QVector<float> getAudioTapSamples() const;  // 获取16kHz mono float32样本

    // 实时字幕输入：定长环形缓冲（默认 60 秒），与离线录制互不影响。
    // 打开后音频线程持续写入，消费者用 audioRingPosition() 记进度、copyAudioRingFrom() 取增量。
    void setAudioRingEnabled(bool enabled);
    bool isAudioRingEnabled() const { return m_audioRingEnabled; }
    qint64 audioRingPosition() const;
    int copyAudioRingFrom(qint64 absStart, float *out, int maxCount) const;

    // 诊断状态（T4.9）
    struct DiagnosticStats {
        QString currentFile;
        qint64 positionMs = 0;
        qint64 durationMs = 0;
        double decodeFps = 0;
        double displayFps = 0;
        int videoFrameQueueSize = 0;
        int audioFrameQueueSize = 0;
        int videoPacketQueueSize = 0;
        int audioPacketQueueSize = 0;
        bool hwDecodeEnabled = false;
        int videoWidth = 0;
        int videoHeight = 0;
        double audioClock = 0;
        double videoClock = 0;
    };
    DiagnosticStats diagnosticStats() const;

signals:
    void frameReady(ID3D11Texture2D *texture, UINT subresource, int width, int height);
    void errorOccurred(const QString &error);
    void durationChanged(qint64 durationMs);
    void positionChanged(qint64 positionMs);
    void seekStarted();  // seek开始时发出，用于清理渲染端pending纹理
    void subtitleChanged(const QString &text);  // 字幕内容变化（空字符串=隐藏字幕）
    void playlistChanged();  // 播放列表变化
    void currentIndexChanged(int index);  // 当前播放索引变化
    void playbackFinished();  // 当前视频播放结束

private slots:
    void displayFrame();
    void performSeek();  // seek 防抖：实际执行 seek

private:
    void demuxLoop();          // 读包线程：只负责读包+分发
    void audioDecodeLoop();    // 音频解码线程
    void videoDecodeLoop();    // 视频解码线程
    void startPlaybackThreads(bool resumePlayback = true);  // 启动三个解码线程；resumePlayback=false 时不动音频/定时器
    void stopPlaybackThreads();   // 停止三个解码线程（不释放解码器/音频设备）
    void signalWorkersToStop();   // 持锁置停止标志并唤醒所有等待中的工作线程
    void joinPlaybackThreads();   // wait + delete 三个线程对象
    void clearPacketQueues();     // 释放两个包队列中的 AVPacket
    void clearFrameQueues();      // 清音频帧队列，视频帧纹理归还池
    bool openCodec(int streamIndex, AVCodecContext *&codecCtx);
    void initAudio(AVCodecContext *audioCodecCtx);
    static void sdlAudioCallback(void *userdata, Uint8 *stream, int len);
    void audioCallback(Uint8 *stream, int len);

    // 软解帧上传到D3D11纹理
    bool ensureSwUploadTexture(int width, int height);
    bool uploadSoftwareFrame(AVFrame *frame, ID3D11Texture2D *&outStagingTex);

    // FFmpeg
    AVFormatContext *m_formatCtx = nullptr;
    AVCodecContext *m_videoCodecCtx = nullptr;
    AVCodecContext *m_audioCodecCtx = nullptr;
    SwrContext *m_swrCtx = nullptr;
    AVBufferRef *m_hwDeviceCtx = nullptr;
    ID3D11Device *m_d3d11Device = nullptr;           // 自创建D3D11设备（解码+显示共用）
    ID3D11DeviceContext *m_d3d11Context = nullptr;   // 自创建D3D11上下文
    // 软解上传中介
    ID3D11Texture2D *m_swUploadTexture = nullptr;    // STAGING NV12 上传中介（NV12 不能用 DYNAMIC）
    SwsContext *m_swsCtx = nullptr;                  // YUV→NV12转换
    int m_swUploadW = 0, m_swUploadH = 0;
    bool m_hwDecodeRequested = true;   // 用户请求硬件解码（可配置）
    bool m_hwDecodeEnabled = false;    // 硬件解码实际生效
    int m_videoStream = -1;
    int m_audioStream = -1;

    // SDL 音频
    SDL_AudioDeviceID m_audioDevice = 0;
    QQueue<QByteArray> m_audioFrameQueue;  // 解码后的音频帧队列
    mutable QMutex m_audioFrameMutex;
    int m_outSampleRate = 44100;
    int m_outChannels = 2;
    int m_outBytesPerSample = 2;
    AVSampleFormat m_outSampleFmt = AV_SAMPLE_FMT_S16;
    float m_volume = 1.0f;  // 当前音量（0.0=静音，1.0=原音量）
    std::atomic<qint64> m_audioPlayedSamples{0};  // SDL实际已播放样本数（音频时钟基准）
    qint64 m_lastClockSamples = 0;    // 上次插值时的样本数
    qint64 m_lastClockTime = 0;       // 上次插值时的系统时间(ms)

    // 诊断统计
    std::atomic<int> m_decodedFrames{0};
    std::atomic<int> m_displayFrames{0};
    double m_lastDecodeFps = 0;
    double m_lastDisplayFps = 0;
    QString m_currentFilePath;
    qint64 m_durationMs = 0;
    int m_videoWidth = 0;
    int m_videoHeight = 0;

    // 音频包队列（读包线程写，音频解码线程读）
    QQueue<AVPacket*> m_audioPacketQueue;
    mutable QMutex m_audioPacketMutex;
    QWaitCondition m_audioPacketCondition;

    // 视频包队列（读包线程写，视频解码线程读）
    QQueue<AVPacket*> m_videoPacketQueue;
    mutable QMutex m_videoPacketMutex;
    QWaitCondition m_videoPacketCondition;

    // 视频帧队列（视频解码线程写，主线程读）— D3D11纹理帧，零拷贝
    QQueue<QPair<VideoFrameData, double>> m_videoFrameQueue;
    mutable QMutex m_frameMutex;
    QWaitCondition m_frameCondition;
    QTimer *m_displayTimer = nullptr;

    // seek 防抖（T4.8 修复：连续 seek 只执行最后一次）
    QTimer *m_seekTimer = nullptr;
    qint64 m_pendingSeekPos = -1;

    // 同步时钟
    double m_seekTime = 0;  // seek 后的起始时间（秒），音频时钟基准偏移

    // 线程控制
    QThread *m_demuxThread = nullptr;
    QThread *m_audioDecodeThread = nullptr;
    QThread *m_videoDecodeThread = nullptr;
    std::atomic<bool> m_isRunning{false};
    std::atomic<bool> m_isPaused{false};
    std::atomic<bool> m_demuxFinished{false};  // demux线程是否已读完文件
    QMutex m_pauseMutex;
    QWaitCondition m_pauseCondition;

    // 字幕
    SubtitleParser m_subtitleParser;
    QString m_currentSubtitleText;

    // 播放列表
    QStringList m_playlist;
    int m_currentIndex = -1;
    bool m_finishedEmitted = false;  // 防止playbackFinished重复发出

    // AudioTap（T4.8）：录制16kHz mono float32音频供whisper识别
    bool m_audioTapEnabled = false;
    SwrContext *m_audioTapSwr = nullptr;
    QVector<float> m_audioTapBuffer;  // 16kHz mono float32（离线录制，按需增长）
    mutable QMutex m_audioTapMutex;

    // 实时字幕：固定容量，避免长时间播放把内存吃光
    bool m_audioRingEnabled = false;
    AudioRingBuffer m_audioRing;
};

#endif // PLAYERCONTROLLER_H
