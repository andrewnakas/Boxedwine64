/* d3d11tri.c -- M29 Direct3D 11 probe: create a device on the best feature
 * level wined3d offers (11_0 .. 9_1) and draw a vertex-colored triangle with
 * fxc-compiled SM4 shaders on a dark blue clear every frame. Every step prints
 * "d3d11tri: <step> hr=0x..." to stderr; "d3d11tri: frame N" proves Present.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -o d3d11tri.exe d3d11tri.c -ld3d11 -ldxgi -luuid -mwindows
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>

static void say(const char *step, HRESULT hr)
{
    fprintf(stderr, "d3d11tri: %s hr=0x%08lx\n", step, (unsigned long)hr);
    fflush(stderr);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

/* fxc-compiled vs_4_0 / ps_4_0 bytecode, taken from Wine's
 * dlls/d3d11/tests/d3d11.c (LGPL). Real D3D10/11 apps ship fxc bytecode,
 * so this is what wined3d must accept. HLSL:
 *   struct vs_out { float4 position : SV_POSITION; float4 color : COLOR; };
 *   vs_out main(float4 position : POSITION, float4 color : COLOR) { o = {position, color}; }
 *   float4 main(vs_out i) : SV_TARGET { return i.color; }
 */
static const DWORD vs_code[] =
{
    0x43425844, 0x5c73b061, 0x5c71125f, 0x3f8b345f, 0xce04b9ab, 0x00000001, 0x00000140, 0x00000003,
    0x0000002c, 0x0000007c, 0x000000d0, 0x4e475349, 0x00000048, 0x00000002, 0x00000008, 0x00000038,
    0x00000000, 0x00000000, 0x00000003, 0x00000000, 0x00000f0f, 0x00000041, 0x00000000, 0x00000000,
    0x00000003, 0x00000001, 0x00000f0f, 0x49534f50, 0x4e4f4954, 0x4c4f4300, 0xab00524f, 0x4e47534f,
    0x0000004c, 0x00000002, 0x00000008, 0x00000038, 0x00000000, 0x00000001, 0x00000003, 0x00000000,
    0x0000000f, 0x00000044, 0x00000000, 0x00000000, 0x00000003, 0x00000001, 0x0000000f, 0x505f5653,
    0x5449534f, 0x004e4f49, 0x4f4c4f43, 0xabab0052, 0x52444853, 0x00000068, 0x00010040, 0x0000001a,
    0x0300005f, 0x001010f2, 0x00000000, 0x0300005f, 0x001010f2, 0x00000001, 0x04000067, 0x001020f2,
    0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000001, 0x05000036, 0x001020f2, 0x00000000,
    0x00101e46, 0x00000000, 0x05000036, 0x001020f2, 0x00000001, 0x00101e46, 0x00000001, 0x0100003e,
};
static const DWORD ps_code[] =
{
    0x43425844, 0xe2087fa6, 0xa35fbd95, 0x8e585b3f, 0x67890f54, 0x00000001, 0x000000f4, 0x00000003,
    0x0000002c, 0x00000080, 0x000000b4, 0x4e475349, 0x0000004c, 0x00000002, 0x00000008, 0x00000038,
    0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x00000044, 0x00000000, 0x00000000,
    0x00000003, 0x00000001, 0x00000f0f, 0x505f5653, 0x5449534f, 0x004e4f49, 0x4f4c4f43, 0xabab0052,
    0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
    0x00000000, 0x0000000f, 0x545f5653, 0x45475241, 0xabab0054, 0x52444853, 0x00000038, 0x00000040,
    0x0000000e, 0x03001062, 0x001010f2, 0x00000001, 0x03000065, 0x001020f2, 0x00000000, 0x05000036,
    0x001020f2, 0x00000000, 0x00101e46, 0x00000001, 0x0100003e,
};

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1,
    };
    static const struct { float x, y, z, r, g, b, a; } verts[] = {
        {  0.0f,  0.8f, 0.5f, 1, 0, 0, 1 },
        {  0.8f, -0.8f, 0.5f, 0, 1, 0, 1 },
        { -0.8f, -0.8f, 0.5f, 0, 0, 1, 1 },
    };
    static const D3D11_INPUT_ELEMENT_DESC layout_desc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    WNDCLASSA wc = {0};
    DXGI_SWAP_CHAIN_DESC sd = {0};
    IDXGISwapChain *swap = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Texture2D *back = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11InputLayout *il = NULL;
    ID3D11Buffer *vb = NULL;
    D3D_FEATURE_LEVEL got = 0;
    D3D11_BUFFER_DESC bd = {0};
    D3D11_SUBRESOURCE_DATA init = {0};
    D3D11_VIEWPORT vp = {0};
    HWND hwnd;
    HRESULT hr;
    MSG msg;
    int frame;

    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "d3d11tri";
    RegisterClassA(&wc);
    hwnd = CreateWindowA("d3d11tri", "d3d11tri", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         40, 40, 640, 480, NULL, NULL, inst, NULL);

    sd.BufferCount = 1;
    sd.BufferDesc.Width = 624;
    sd.BufferDesc.Height = 441;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels,
                                       sizeof(levels) / sizeof(levels[0]), D3D11_SDK_VERSION,
                                       &sd, &swap, &dev, &got, &ctx);
    say("D3D11CreateDeviceAndSwapChain", hr);
    if (FAILED(hr)) return 1;
    fprintf(stderr, "d3d11tri: feature level 0x%x\n", got);

    hr = IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back); say("GetBuffer", hr);
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv); say("CreateRenderTargetView", hr);
    if (FAILED(hr)) return 1;

    hr = ID3D11Device_CreateVertexShader(dev, vs_code, sizeof(vs_code), NULL, &vs); say("CreateVertexShader", hr);
    hr = ID3D11Device_CreatePixelShader(dev, ps_code, sizeof(ps_code), NULL, &ps); say("CreatePixelShader", hr);
    hr = ID3D11Device_CreateInputLayout(dev, layout_desc, 2, vs_code, sizeof(vs_code), &il); say("CreateInputLayout", hr);
    bd.ByteWidth = sizeof(verts);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    init.pSysMem = verts;
    hr = ID3D11Device_CreateBuffer(dev, &bd, &init, &vb); say("CreateBuffer(vertices)", hr);

    vp.Width = 624; vp.Height = 441; vp.MaxDepth = 1.0f;
    for (frame = 0;; frame++) {
        static const float blue[4] = { 0.0f, 0.125f, 0.375f, 1.0f };
        UINT stride = sizeof(verts[0]), offset = 0;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, blue);
        if (vs && ps && il && vb) {
            ID3D11DeviceContext_IASetInputLayout(ctx, il);
            ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
        }
        hr = IDXGISwapChain_Present(swap, 0, 0);
        if (frame == 0) say("Present", hr);
        if (frame % 60 == 0) { fprintf(stderr, "d3d11tri: frame %d\n", frame); fflush(stderr); }
        Sleep(16);
    }
}
