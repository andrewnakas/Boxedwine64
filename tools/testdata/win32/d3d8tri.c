/* d3d8tri.c -- M28 Direct3D 8 probe: a windowed device drawing a
 * vertex-colored, pre-transformed triangle (DrawPrimitiveUP) on a dark blue
 * clear, presented every frame. Steps print "d3d8tri: <step> hr=0x..." to
 * stderr; "d3d8tri: frame N" proves Present keeps working.
 *
 * Build: i686-w64-mingw32-gcc -O2 -o d3d8tri.exe d3d8tri.c -ld3d8 -mwindows
 */
#include <windows.h>
#include <d3d8.h>
#include <stdio.h>

struct vtx { float x, y, z, rhw; DWORD color; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static void say(const char *step, HRESULT hr)
{
    fprintf(stderr, "d3d8tri: %s hr=0x%08lx\n", step, (unsigned long)hr);
    fflush(stderr);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc = {0};
    HWND hwnd;
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev = NULL;
    D3DDISPLAYMODE mode;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    RECT rc;
    MSG msg;
    int frame;

    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "d3d8tri";
    RegisterClassA(&wc);
    hwnd = CreateWindowA("d3d8tri", "d3d8tri", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         40, 40, 640, 480, NULL, NULL, inst, NULL);

    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    say("Direct3DCreate8", d3d ? S_OK : E_FAIL);
    if (!d3d) return 1;
    hr = IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode); say("GetAdapterDisplayMode", hr);

    memset(&pp, 0, sizeof pp);
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = mode.Format;
    hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    say("CreateDevice", hr);
    if (FAILED(hr)) return 1;
    IDirect3DDevice8_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);

    for (frame = 0;; frame++) {
        struct vtx v[3];
        float w, h;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        GetClientRect(hwnd, &rc);
        w = (float)rc.right; h = (float)rc.bottom;
        v[0] = (struct vtx){ w * 0.5f, h * 0.1f, 0.5f, 1.0f, 0xffff0000 };
        v[1] = (struct vtx){ w * 0.9f, h * 0.9f, 0.5f, 1.0f, 0xff00ff00 };
        v[2] = (struct vtx){ w * 0.1f, h * 0.9f, 0.5f, 1.0f, 0xff0000ff };

        hr = IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 32, 96), 1.0f, 0);
        if (frame == 0) say("Clear", hr);
        hr = IDirect3DDevice8_BeginScene(dev);
        if (frame == 0) say("BeginScene", hr);
        IDirect3DDevice8_SetVertexShader(dev, FVF);
        hr = IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, v, sizeof v[0]);
        if (frame == 0) say("DrawPrimitiveUP", hr);
        IDirect3DDevice8_EndScene(dev);
        hr = IDirect3DDevice8_Present(dev, NULL, NULL, NULL, NULL);
        if (frame == 0) say("Present", hr);
        if (frame % 60 == 0) { fprintf(stderr, "d3d8tri: frame %d\n", frame); fflush(stderr); }
        Sleep(16);
    }
}
