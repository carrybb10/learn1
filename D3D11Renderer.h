#ifndef D3D11RENDERER_H
#define D3D11RENDERER_H

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <QMutex>
#include <QQueue>
#include <QImage>

using Microsoft::WRL::ComPtr;

// 保护D3D11立即上下文的全局互斥锁（解码线程拷贝+渲染线程渲染都需锁定）
extern QMutex g_d3d11Mutex;

// 中转纹理池：预分配NV12纹理循环使用，避免每帧分配/释放GPU内存
class StagingTexturePool
{
public:
    static StagingTexturePool& instance();
    ID3D11Texture2D* acquire(ID3D11Device *device, int width, int height);
    void release(ID3D11Texture2D *texture);
    void clear();

private:
    StagingTexturePool() = default;
    ~StagingTexturePool() { clear(); }
    QMutex m_mutex;
    QQueue<ID3D11Texture2D*> m_pool;
    int m_width = 0;
    int m_height = 0;
};

class D3D11Renderer
{
public:
    D3D11Renderer();
    ~D3D11Renderer();

    bool init(HWND hwnd, int width, int height);
    bool initWithDevice(ID3D11Device *device, HWND hwnd, int width, int height);
    void renderFrame(ID3D11Texture2D *texture, UINT subresource, int frameWidth, int frameHeight);
    void resize(int width, int height);
    void flush();  // 提交所有挂起的GPU命令
    void release();
    bool isValid() const { return m_device != nullptr; }
    QImage capture();  // 截取当前画面，返回QImage（失败返回空QImage）

private:
    bool createShaders();
    bool createRenderTarget();
    void cleanupRenderTarget();

    HWND m_hwnd = nullptr;
    int m_width = 0;
    int m_height = 0;

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_renderTarget;
    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11InputLayout> m_inputLayout;
    ComPtr<ID3D11Buffer> m_vertexBuffer;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11RasterizerState> m_rasterizer;
    ComPtr<ID3D11Texture2D> m_stagingTexture;  // 中转纹理（带SHADER_RESOURCE标志）
    int m_stagingWidth = 0;
    int m_stagingHeight = 0;
};

#endif // D3D11RENDERER_H
