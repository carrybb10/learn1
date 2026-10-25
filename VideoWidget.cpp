#include "VideoWidget.h"
#include <QExposeEvent>
#include <QResizeEvent>
#include <QDebug>

VideoWidget::VideoWidget(QWindow *parent)
    : QWindow(parent)
{
    setMinimumSize(QSize(320, 240));
}

VideoWidget::~VideoWidget()
{
    if (m_pendingTexture) {
        StagingTexturePool::instance().release(m_pendingTexture);
        m_pendingTexture = nullptr;
    }
    m_renderer.release();
}

void VideoWidget::ensureInit(ID3D11Texture2D *fromTexture)
{
    // 若渲染器已用独立设备初始化，但视频帧来自解码设备，需重新初始化
    if (m_initialized && fromTexture && !m_usingDecoderDevice) {
        m_renderer.release();
        m_initialized = false;
        qDebug() << "[VideoWindow] Re-init with decoder device (was own device)";
    }
    if (m_initialized) return;
    if (!isExposed()) return;

    HWND hwnd = (HWND)winId();
    if (!hwnd) return;

    if (fromTexture) {
        ID3D11Device *device = nullptr;
        fromTexture->GetDevice(&device);
        if (device) {
            if (m_renderer.initWithDevice(device, hwnd, width(), height())) {
                m_initialized = true;
                m_usingDecoderDevice = true;
                qDebug() << "[VideoWindow] Initialized with decoder device";
            }
            device->Release();
        }
    }

    if (!m_initialized) {
        if (m_renderer.init(hwnd, width(), height())) {
            m_initialized = true;
            m_usingDecoderDevice = false;
            qDebug() << "[VideoWindow] Initialized with own device";
        }
    }

    if (m_initialized && m_pendingTexture) {
        m_renderer.renderFrame(m_pendingTexture, m_pendingSubresource, m_pendingWidth, m_pendingHeight);
        StagingTexturePool::instance().release(m_pendingTexture);
        m_pendingTexture = nullptr;
    }
}

void VideoWidget::exposeEvent(QExposeEvent *event)
{
    Q_UNUSED(event);
    // 渲染器未初始化时，用GDI填充黑色背景（QWindow默认白色，QSS对其无效）
    if (!m_initialized && isExposed()) {
        HDC hdc = GetDC((HWND)winId());
        if (hdc) {
            RECT rect;
            GetClientRect((HWND)winId(), &rect);  // 用实际物理像素大小，避免高DPI缩放问题
            FillRect(hdc, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
            ReleaseDC((HWND)winId(), hdc);
        }
    }
    if (isExposed() && m_pendingTexture) {
        ensureInit(m_pendingTexture);
    }
}

void VideoWidget::resizeEvent(QResizeEvent *event)
{
    QWindow::resizeEvent(event);
    qDebug() << "[VideoWidget] resizeEvent:" << width() << "x" << height() << "initialized:" << m_initialized;
    if (m_initialized) {
        m_renderer.resize(width(), height());
    } else if (isExposed()) {
        // 渲染器未初始化时resize，用GDI重新填充黑色
        HDC hdc = GetDC((HWND)winId());
        if (hdc) {
            RECT rect;
            GetClientRect((HWND)winId(), &rect);
            FillRect(hdc, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
            ReleaseDC((HWND)winId(), hdc);
        }
    }
}

void VideoWidget::clearPending()
{
    if (m_pendingTexture) {
        StagingTexturePool::instance().release(m_pendingTexture);
        m_pendingTexture = nullptr;
    }
}

void VideoWidget::reset()
{
    clearPending();
    if (m_initialized) {
        m_renderer.flush();  // 确保GPU命令提交完成
    }
    m_renderer.release();
    m_initialized = false;
    m_usingDecoderDevice = false;
    StagingTexturePool::instance().clear();  // 释放旧设备的所有中转纹理（必须在设备释放后、新设备创建前）
}

QImage VideoWidget::capture()
{
    if (!m_initialized) return QImage();
    QMutexLocker locker(&g_d3d11Mutex);
    return m_renderer.capture();
}

void VideoWidget::setFrame(ID3D11Texture2D *texture, UINT subresource, int width, int height)
{
    // 总是调用ensureInit：内部会检测设备不匹配并重新初始化（独立设备→解码设备）
    ensureInit(texture);
    if (!m_initialized) {
        if (m_pendingTexture) StagingTexturePool::instance().release(m_pendingTexture);
        m_pendingTexture = texture;
        m_pendingSubresource = subresource;
        m_pendingWidth = width;
        m_pendingHeight = height;
        return;
    }
    m_renderer.renderFrame(texture, subresource, width, height);
    StagingTexturePool::instance().release(texture);  // 归还纹理池，循环使用
}
