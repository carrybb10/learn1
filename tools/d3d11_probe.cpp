// d3d11_probe: 复现软解上传链路，不依赖 GUI / 视频文件。
// 目的：把"软解黑屏"变成一条可断言的命令，能在 CI 与本机反复跑。
//
// 链路：NV12 上传纹理 -> CopySubresourceRegion -> 池纹理(DEFAULT+SRV) -> 采样渲染 -> 回读像素。
// 每一步打印 HRESULT 与结论，最后判断回读画面是否为非黑。
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>

static const UINT kWidth = 64;
static const UINT kHeight = 48;

struct ComGuard {
    template <typename T> static void release(T *&p) { if (p) { p->Release(); p = nullptr; } }
};

static const char *kVS = R"(
struct VS_IN { float2 pos : POSITION; float2 uv : TEXCOORD; };
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VS_OUT main(VS_IN i) { VS_OUT o; o.pos = float4(i.pos, 0, 1); o.uv = i.uv; return o; }
)";

static const char *kPS = R"(
Texture2D<float>  yTex  : register(t0);
Texture2D<float2> uvTex : register(t1);
SamplerState samp : register(s0);
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(VS_OUT i) : SV_TARGET {
    float y = yTex.Sample(samp, i.uv).r;
    float2 uv = uvTex.Sample(samp, i.uv).rg - 0.5;
    return float4(y + 1.5748 * uv.g, y - 0.1873 * uv.r - 0.4681 * uv.g, y + 1.8556 * uv.r, 1);
}
)";

static int g_failures = 0;
static void Check(bool ok, const char *what, HRESULT hr = S_OK)
{
    if (ok) {
        std::printf("  [ok]   %s\n", what);
    } else {
        std::printf("  [FAIL] %s (hr=0x%08lX)\n", what, (unsigned long)hr);
        g_failures++;
    }
}

// 把一段 NV12 数据写进纹理。dynamic 用 WRITE_DISCARD，staging 只能用 WRITE。
static HRESULT FillNv12(ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, bool dynamic, BYTE y, BYTE uv)
{
    D3D11_MAPPED_SUBRESOURCE m = {};
    HRESULT hr = ctx->Map(tex, 0, dynamic ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE, 0, &m);
    if (FAILED(hr)) return hr;
    BYTE *dst = (BYTE *)m.pData;
    for (UINT r = 0; r < kHeight; r++) memset(dst + r * m.RowPitch, y, kWidth);
    BYTE *uvDst = dst + m.RowPitch * kHeight;
    for (UINT r = 0; r < kHeight / 2; r++) memset(uvDst + r * m.RowPitch, uv, kWidth);
    ctx->Unmap(tex, 0);
    return S_OK;
}

int main()
{
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, &fl, &ctx);
    const char *driver = "hardware";
    if (FAILED(hr)) {
        // CI 上的 windows-latest 没有真实 GPU，退回 WARP 软件光栅器，链路逻辑一致
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, &device, &fl, &ctx);
        driver = "warp";
    }
    if (FAILED(hr)) {
        std::printf("FATAL: D3D11CreateDevice failed hr=0x%08lX\n", (unsigned long)hr);
        return 2;
    }
    std::printf("device created via %s, feature level 0x%X\n", driver, (unsigned int)fl);

    D3D11_TEXTURE2D_DESC upload = {};
    upload.Width = kWidth;
    upload.Height = kHeight;
    upload.MipLevels = 1;
    upload.ArraySize = 1;
    upload.Format = DXGI_FORMAT_NV12;
    upload.SampleDesc.Count = 1;

    // ---- 1. 当前代码用的 DYNAMIC NV12 上传纹理 ----
    D3D11_TEXTURE2D_DESC dynamicDesc = upload;
    dynamicDesc.Usage = D3D11_USAGE_DYNAMIC;
    dynamicDesc.BindFlags = 0;
    dynamicDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D *dynamicTex = nullptr;
    hr = device->CreateTexture2D(&dynamicDesc, nullptr, &dynamicTex);
    // 记录而非断言：D3D11 不允许 NV12 + DYNAMIC（E_INVALIDARG）。这条路径曾让软解全黑，
    // 保留记录是为了下次换驱动/换格式时能立刻看出"到底是不是它"。
    std::printf("  [info] NV12 DYNAMIC create -> hr=0x%08lX (%s)\n", (unsigned long)hr,
                SUCCEEDED(hr) ? "accepted" : "rejected, as expected");
    if (SUCCEEDED(hr)) {
        HRESULT mhr = FillNv12(ctx, dynamicTex, true, 128, 128);
        std::printf("  [info] Map(WRITE_DISCARD) on DYNAMIC NV12 -> hr=0x%08lX\n", (unsigned long)mhr);
    }

    // ---- 2. STAGING 版本（备选方案） ----
    D3D11_TEXTURE2D_DESC stagingDesc = upload;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D *stagingTex = nullptr;
    hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
    Check(SUCCEEDED(hr), "create NV12 STAGING upload texture", hr);
    if (SUCCEEDED(hr)) {
        HRESULT mhr = FillNv12(ctx, stagingTex, false, 128, 128);
        Check(SUCCEEDED(mhr), "Map(WRITE) on STAGING NV12", mhr);
    }

    // ---- 3. 池纹理：DEFAULT + SHADER_RESOURCE ----
    D3D11_TEXTURE2D_DESC poolDesc = upload;
    poolDesc.Usage = D3D11_USAGE_DEFAULT;
    poolDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *poolTex = nullptr;
    hr = device->CreateTexture2D(&poolDesc, nullptr, &poolTex);
    Check(SUCCEEDED(hr), "create pool NV12 DEFAULT+SRV texture", hr);
    if (FAILED(hr)) return 1;

    // ---- 4. 拷贝：两个来源各试一次 ----
    for (int i = 0; i < 2; i++) {
        ID3D11Texture2D *src = (i == 0) ? dynamicTex : stagingTex;
        if (!src) continue;
        std::printf("copy from %s:\n", (i == 0) ? "DYNAMIC" : "STAGING");
        __try {
            ctx->CopySubresourceRegion(poolTex, 0, 0, 0, 0, src, 0, nullptr);
            ctx->Flush();
            Check(true, "CopySubresourceRegion did not raise");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Check(false, "CopySubresourceRegion raised an exception");
        }
    }

    // ---- 4.5 read back the pool texture Y plane: did the copy actually move data? ----
    {
        D3D11_TEXTURE2D_DESC rd = upload;
        rd.Usage = D3D11_USAGE_STAGING;
        rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D *poolRead = nullptr;
        HRESULT rhr = device->CreateTexture2D(&rd, nullptr, &poolRead);
        Check(SUCCEEDED(rhr), "create NV12 readback texture", rhr);
        if (SUCCEEDED(rhr)) {
            ctx->CopyResource(poolRead, poolTex);
            D3D11_MAPPED_SUBRESOURCE pm = {};
            rhr = ctx->Map(poolRead, 0, D3D11_MAP_READ, 0, &pm);
            Check(SUCCEEDED(rhr), "map NV12 readback", rhr);
            if (SUCCEEDED(rhr)) {
                BYTE *pp = (BYTE *)pm.pData;
                std::printf("  pool Y[0]=%u Y[mid]=%u UV[0]=%u rowPitch=%u\n",
                            pp[0], pp[pm.RowPitch * (kHeight / 2)], pp[pm.RowPitch * kHeight], pm.RowPitch);
                ctx->Unmap(poolRead, 0);
            }
            poolRead->Release();
        }
    }
    // ---- 5. 着色器 + 采样 + 回读 ----
    ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *err = nullptr;
    hr = D3DCompile(kVS, strlen(kVS), "VS", nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsBlob, &err);
    Check(SUCCEEDED(hr), "compile vertex shader", hr);
    hr = D3DCompile(kPS, strlen(kPS), "PS", nullptr, nullptr, "main", "ps_4_0", 0, 0, &psBlob, &err);
    Check(SUCCEEDED(hr), "compile pixel shader", hr);
    if (!vsBlob || !psBlob) return 1;

    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

    struct V { float x, y, u, v; };
    V verts[] = { {-1,-1,0,1}, {1,-1,1,1}, {-1,1,0,0}, {1,-1,1,1}, {1,1,1,0}, {-1,1,0,0} };
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = sizeof(verts);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = { verts, 0, 0 };
    ID3D11Buffer *vb = nullptr;
    device->CreateBuffer(&bd, &sd, &vb);

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *il = nullptr;
    device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &il);

    D3D11_SAMPLER_DESC samp = {};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    // D3D11 default rasterizer culls back faces; the app sets CULL_NONE for the same reason.
    D3D11_RASTERIZER_DESC rdesc = {};
    rdesc.CullMode = D3D11_CULL_NONE;
    rdesc.FillMode = D3D11_FILL_SOLID;
    ID3D11RasterizerState *rs = nullptr;
    device->CreateRasterizerState(&rdesc, &rs);

    ID3D11SamplerState *ss = nullptr;
    device->CreateSamplerState(&samp, &ss);

    D3D11_SHADER_RESOURCE_VIEW_DESC yDesc = {};
    yDesc.Format = DXGI_FORMAT_R8_UNORM;
    yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    yDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView *ySrv = nullptr;
    hr = device->CreateShaderResourceView(poolTex, &yDesc, &ySrv);
    Check(SUCCEEDED(hr), "create Y SRV (R8_UNORM)", hr);

    D3D11_SHADER_RESOURCE_VIEW_DESC uvDesc = yDesc;
    uvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    ID3D11ShaderResourceView *uvSrv = nullptr;
    hr = device->CreateShaderResourceView(poolTex, &uvDesc, &uvSrv);
    Check(SUCCEEDED(hr), "create UV SRV (R8G8_UNORM)", hr);

    D3D11_TEXTURE2D_DESC rtDesc = {};
    rtDesc.Width = kWidth;
    rtDesc.Height = kHeight;
    rtDesc.MipLevels = 1;
    rtDesc.ArraySize = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Usage = D3D11_USAGE_DEFAULT;
    rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = nullptr;
    device->CreateTexture2D(&rtDesc, nullptr, &rt);
    ID3D11RenderTargetView *rtv = nullptr;
    device->CreateRenderTargetView(rt, nullptr, &rtv);

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)kWidth, (float)kHeight, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs);
    float clear[4] = { 0, 0, 0, 1 };
    ctx->ClearRenderTargetView(rtv, clear);
    UINT stride = sizeof(V), off = 0;
    ctx->IASetInputLayout(il);
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &ySrv);
    ctx->PSSetShaderResources(1, 1, &uvSrv);
    ctx->PSSetSamplers(0, 1, &ss);
    ctx->Draw(6, 0);

    D3D11_TEXTURE2D_DESC readDesc = rtDesc;
    readDesc.Usage = D3D11_USAGE_STAGING;
    readDesc.BindFlags = 0;
    readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *readTex = nullptr;
    device->CreateTexture2D(&readDesc, nullptr, &readTex);
    ctx->CopyResource(readTex, rt);
    D3D11_MAPPED_SUBRESOURCE m = {};
    hr = ctx->Map(readTex, 0, D3D11_MAP_READ, 0, &m);
    Check(SUCCEEDED(hr), "map readback texture", hr);
    if (SUCCEEDED(hr)) {
        BYTE *p = (BYTE *)m.pData;
        double sum = 0;
        for (UINT y = 0; y < kHeight; y++) {
            for (UINT x = 0; x < kWidth; x++) {
                BYTE *px = p + y * m.RowPitch + x * 4;
                sum += (px[0] + px[1] + px[2]) / 3.0;
            }
        }
        double avg = sum / (kWidth * kHeight);
        std::printf("  readback average luma = %.1f (expected ~128)\n", avg);
        Check(avg > 20.0, "rendered frame is not black");
        ctx->Unmap(readTex, 0);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures ? "PROBE FAILED" : "PROBE PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
