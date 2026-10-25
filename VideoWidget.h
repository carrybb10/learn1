#ifndef VIDEOWIDGET_H
#define VIDEOWIDGET_H

#include <QWindow>
#include <QImage>
#include <d3d11.h>
#include "D3D11Renderer.h"

class VideoWidget : public QWindow
{
    Q_OBJECT
public:
    explicit VideoWidget(QWindow *parent = nullptr);
    ~VideoWidget();

    void setFrame(ID3D11Texture2D *texture, UINT subresource, int width, int height);
    void clearPending();  // seek/stop时清理待渲染纹理（归还池中）
    void reset();  // 打开新文件前重置渲染器（释放旧设备的交换链）
    QImage capture();  // 截取当前画面

protected:
    void exposeEvent(QExposeEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void ensureInit(ID3D11Texture2D *fromTexture = nullptr);

    D3D11Renderer m_renderer;
    bool m_initialized = false;
    bool m_usingDecoderDevice = false;  // 渲染器是否复用解码设备（独立设备初始化时为false）
    ID3D11Texture2D *m_pendingTexture = nullptr;
    UINT m_pendingSubresource = 0;
    int m_pendingWidth = 0;
    int m_pendingHeight = 0;
};

#endif // VIDEOWIDGET_H
