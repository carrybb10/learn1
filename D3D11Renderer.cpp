#include "D3D11Renderer.h"
#include <QDebug>

QMutex g_d3d11Mutex;

// === StagingTexturePool 实现 ===
StagingTexturePool& StagingTexturePool::instance()
{
    static StagingTexturePool s_instance;
    return s_instance;
}

ID3D11Texture2D* StagingTexturePool::acquire(ID3D11Device *device, int width, int height)
{
    QMutexLocker locker(&m_mutex);
    // 尺寸变化时清空旧池
    if ((m_width != width || m_height != height) && !m_pool.isEmpty()) {
        while (!m_pool.isEmpty()) {
            m_pool.dequeue()->Release();
        }
        m_width = width;
        m_height = height;
    }
    if (!m_pool.isEmpty()) {
        return m_pool.dequeue();
    }
    // 池空，创建新纹理
    D3D11_TEXTURE2D_DESC std = {};
    std.Width = width;
    std.Height = height;
    std.MipLevels = 1;
    std.ArraySize = 1;
    std.Format = DXGI_FORMAT_NV12;
    std.SampleDesc.Count = 1;
    std.Usage = D3D11_USAGE_DEFAULT;
    std.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *tex = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&std, nullptr, &tex))) {
        m_width = width;
        m_height = height;
    }
    return tex;
}

void StagingTexturePool::release(ID3D11Texture2D *texture)
{
    if (!texture) return;
    QMutexLocker locker(&m_mutex);
    m_pool.enqueue(texture);
}

void StagingTexturePool::clear()
{
    QMutexLocker locker(&m_mutex);
    while (!m_pool.isEmpty()) {
        m_pool.dequeue()->Release();
    }
    m_width = 0;
    m_height = 0;
}

// 全屏四边形顶点：位置(ND C) + UV
struct Vertex {
    float x, y;
    float u, v;
};

static const char *kVertexShader = R"(
struct VS_IN { float2 pos : POSITION; float2 uv : TEXCOORD; };
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VS_OUT main(VS_IN input) {
    VS_OUT o;
    o.pos = float4(input.pos, 0.0, 1.0);
    o.uv = input.uv;
    return o;
}
)";

// NV12 YUV→RGB (BT.709)
static const char *kPixelShader = R"(
Texture2D<float>  yTex  : register(t0);
Texture2D<float2> uvTex : register(t1);
SamplerState samp : register(s0);
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(VS_OUT input) : SV_TARGET {
    float y = yTex.Sample(samp, input.uv).r;
    float2 uv = uvTex.Sample(samp, input.uv).rg - 0.5;
    float r = y + 1.5748 * uv.g;
    float g = y - 0.1873 * uv.r - 0.4681 * uv.g;
    float b = y + 1.8556 * uv.r;
    return float4(r, g, b, 1.0);
}
)";

D3D11Renderer::D3D11Renderer() {}

D3D11Renderer::~D3D11Renderer()
{
    release();
}

bool D3D11Renderer::init(HWND hwnd, int width, int height)
{
    return initWithDevice(nullptr, hwnd, width, height);
}

bool D3D11Renderer::initWithDevice(ID3D11Device *device, HWND hwnd, int width, int height)
{
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    if (device) {
        // 复用外部设备（如FFmpeg D3D11VA解码器的设备）
        m_device = device;
        device->GetImmediateContext(&m_context);

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            qWarning() << "Failed to get DXGI device";
            return false;
        }
        ComPtr<IDXGIAdapter> adapter;
        dxgiDevice->GetAdapter(&adapter);
        ComPtr<IDXGIFactory> factory;
        adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(factory->CreateSwapChain(device, &scd, &m_swapChain))) {
            qWarning() << "Failed to create swap chain from external device";
            return false;
        }
        qDebug() << "[D3D11] Swap chain created from external device";
    } else {
        UINT createFlags = 0;
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL featureLevel;

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
            featureLevels, 1, D3D11_SDK_VERSION,
            &scd, &m_swapChain, &m_device, &featureLevel, &m_context);

        if (FAILED(hr)) {
            qWarning() << "D3D11CreateDeviceAndSwapChain failed:" << hr;
            return false;
        }
        qDebug() << "D3D11 renderer initialized, feature level:" << featureLevel;
    }

    if (!createRenderTarget()) return false;
    if (!createShaders()) return false;

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    m_device->CreateSamplerState(&sampDesc, &m_sampler);

    // 禁用背面剔除
    D3D11_RASTERIZER_DESC rastDesc = {};
    rastDesc.CullMode = D3D11_CULL_NONE;
    rastDesc.FillMode = D3D11_FILL_SOLID;
    m_device->CreateRasterizerState(&rastDesc, &m_rasterizer);

    return true;
}

bool D3D11Renderer::createRenderTarget()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) {
        qWarning() << "GetBuffer failed:" << hr;
        return false;
    }
    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_renderTarget);
    if (FAILED(hr)) {
        qWarning() << "CreateRenderTargetView failed:" << hr;
        return false;
    }
    m_context->OMSetRenderTargets(1, m_renderTarget.GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
    return true;
}

bool D3D11Renderer::createShaders()
{
    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;

    HRESULT hr = D3DCompile(kVertexShader, strlen(kVertexShader), "VS", nullptr, nullptr,
        "main", "vs_4_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        qWarning() << "VS compile failed:" << (char*)errorBlob->GetBufferPointer();
        return false;
    }

    hr = D3DCompile(kPixelShader, strlen(kPixelShader), "PS", nullptr, nullptr,
        "main", "ps_4_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        qWarning() << "PS compile failed:" << (char*)errorBlob->GetBufferPointer();
        return false;
    }

    m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
    m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout);

    // 全屏四边形
    Vertex vertices[] = {
        { -1, -1, 0, 1 }, {  1, -1, 1, 1 }, { -1,  1, 0, 0 },
        {  1, -1, 1, 1 }, {  1,  1, 1, 0 }, { -1,  1, 0, 0 },
    };
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = sizeof(vertices);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = { vertices, 0, 0 };
    m_device->CreateBuffer(&bd, &sd, &m_vertexBuffer);

    return true;
}

void D3D11Renderer::renderFrame(ID3D11Texture2D *texture, UINT subresource, int frameWidth, int frameHeight)
{
    if (!m_device || !texture) return;

    QMutexLocker locker(&g_d3d11Mutex);

    // 设置渲染目标和视口
    m_context->OMSetRenderTargets(1, m_renderTarget.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
    if (m_rasterizer) m_context->RSSetState(m_rasterizer.Get());

    // 先清除为黑色（即使后续SRV创建失败也能看到黑色背景，而非残留内容）
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_context->ClearRenderTargetView(m_renderTarget.Get(), clearColor);

    // 创建Y SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC yDesc = {};
    yDesc.Format = DXGI_FORMAT_R8_UNORM;
    yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    yDesc.Texture2D.MipLevels = 1;
    yDesc.Texture2D.MostDetailedMip = 0;
    ComPtr<ID3D11ShaderResourceView> ySrv;
    if (FAILED(m_device->CreateShaderResourceView(texture, &yDesc, &ySrv))) {
        qWarning() << "[D3D11] Create Y SRV failed";
        m_swapChain->Present(0, 0);
        return;
    }

    // 创建UV SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC uvDesc = {};
    uvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    uvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    uvDesc.Texture2D.MipLevels = 1;
    uvDesc.Texture2D.MostDetailedMip = 0;
    ComPtr<ID3D11ShaderResourceView> uvSrv;
    if (FAILED(m_device->CreateShaderResourceView(texture, &uvDesc, &uvSrv))) {
        qWarning() << "[D3D11] Create UV SRV failed";
        m_swapChain->Present(0, 0);
        return;
    }

    m_context->IASetInputLayout(m_inputLayout.Get());
    UINT stride = sizeof(Vertex), offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, ySrv.GetAddressOf());
    m_context->PSSetShaderResources(1, 1, uvSrv.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_context->Draw(6, 0);

    m_swapChain->Present(0, 0);  // 关闭垂直同步，支持高帧率视频
    m_context->Flush();  // 确保命令提交到GPU，减少纹理被并发访问导致TDR的风险
}

void D3D11Renderer::resize(int width, int height)
{
    if (!m_swapChain || width <= 0 || height <= 0) return;
    m_width = width;
    m_height = height;
    // ResizeBuffers前必须彻底重置context状态，否则DXGI_ERROR_INVALID_CALL
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    cleanupRenderTarget();
    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        qWarning() << "[D3D11] ResizeBuffers failed:" << hr << "size:" << width << "x" << height;
        return;
    }
    createRenderTarget();
    // 立即清除为黑色并Present(0,0立即显示)，避免resize后垂直同步延迟导致白色背景
    if (m_renderTarget) {
        float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        m_context->ClearRenderTargetView(m_renderTarget.Get(), black);
        m_swapChain->Present(0, 0);
    }
    qDebug() << "[D3D11] Resized to" << width << "x" << height;
}

void D3D11Renderer::cleanupRenderTarget()
{
    m_renderTarget.Reset();
}

void D3D11Renderer::flush()
{
    if (m_context) m_context->Flush();
}

QImage D3D11Renderer::capture()
{
    if (!m_swapChain || !m_device || !m_context) return QImage();

    // 获取back buffer
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuffer))) {
        qWarning() << "[D3D11] capture: GetBuffer failed";
        return QImage();
    }

    D3D11_TEXTURE2D_DESC desc;
    backBuffer->GetDesc(&desc);

    // 创建staging texture（CPU可读）
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = desc.Width;
    stagingDesc.Height = desc.Height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = desc.Format;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> stagingTex;
    if (FAILED(m_device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex))) {
        qWarning() << "[D3D11] capture: Create staging texture failed";
        return QImage();
    }

    // 拷贝back buffer到staging
    m_context->CopyResource(stagingTex.Get(), backBuffer.Get());

    // Map读取像素
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(m_context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        qWarning() << "[D3D11] capture: Map failed";
        return QImage();
    }

    // 转换为QImage（back buffer是R8G8B8A8，用Format_RGBA8888直接对应）
    QImage image(desc.Width, desc.Height, QImage::Format_RGBA8888);
    uint8_t *src = static_cast<uint8_t*>(mapped.pData);
    for (UINT y = 0; y < desc.Height; y++) {
        memcpy(image.scanLine(y), src + y * mapped.RowPitch, desc.Width * 4);
    }

    m_context->Unmap(stagingTex.Get(), 0);
    return image;
}

void D3D11Renderer::release()
{
    cleanupRenderTarget();
    m_sampler.Reset();
    m_vertexBuffer.Reset();
    m_inputLayout.Reset();
    m_vertexShader.Reset();
    m_pixelShader.Reset();
    m_context.Reset();
    m_swapChain.Reset();
    m_device.Reset();
    m_hwnd = nullptr;
}
