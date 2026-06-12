/*
 * d3dtri.c — minimal Win32 Direct3D9 "first light" test for Boxedwine64.
 *
 * Opens a window, creates a D3D9 device, and renders a colored triangle on a
 * blue background using the FIXED-FUNCTION pipeline (FVF vertices, no HLSL).
 * This exercises wine's wined3d -> opengl32 -> guest libGL.so.1 -> host gl64
 * bridge path. wined3d translates D3D draws into GL; with only fixed-function
 * GL in the bridge today this is expected to render nothing useful at first —
 * the point is to capture (via the libGL proc-address trace) exactly which GL
 * entry points wined3d resolves, so we can implement them.
 *
 * Build (mingw-w64):
 *   x86_64-w64-mingw32-gcc -O2 -o d3dtri.exe d3dtri.c -ld3d9 -lgdi32 -luser32
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

// Print diagnostics to STDERR so they show in the Boxedwine console (the guest's
// fd=2 is piped through). OutputDebugStringA alone is invisible without +relay.
static void dbg(const char* s) {
    fprintf(stderr, "%s", s); fflush(stderr);
    OutputDebugStringA(s);
}

typedef struct { float x, y, z, rhw; DWORD color; } CUSTOMVERTEX;
#define D3DFVF_CUSTOMVERTEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static IDirect3D9*       g_d3d  = NULL;
static IDirect3DDevice9* g_dev  = NULL;
static IDirect3DVertexBuffer9* g_vb = NULL;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

static int initD3D(HWND hwnd) {
    dbg("d3dtri: calling Direct3DCreate9...\n");
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) { dbg("d3dtri: Direct3DCreate9 FAILED (returned NULL)\n"); return 0; }
    dbg("d3dtri: Direct3DCreate9 OK; creating device...\n");

    D3DPRESENT_PARAMETERS pp; ZeroMemory(&pp, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D16;
    // Dodge any vsync/frame-wait in Present (the windowed-present hang probe).
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    HRESULT hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
        hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_dev);
    if (FAILED(hr)) {
        char buf[128]; wsprintfA(buf, "d3dtri: CreateDevice HAL FAILED hr=0x%08lx, trying REF\n", hr);
        dbg(buf);
        hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_REF,
            hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_dev);
        if (FAILED(hr)) { char b2[128]; wsprintfA(b2,"d3dtri: CreateDevice REF FAILED too hr=0x%08lx\n",hr); dbg(b2); return 0; }
    }
    dbg("d3dtri: device created OK\n");

    CUSTOMVERTEX verts[] = {
        { 240.0f,  60.0f, 0.5f, 1.0f, 0xffff0000 },   // top    red
        { 420.0f, 300.0f, 0.5f, 1.0f, 0xff00ff00 },   // right  green
        {  60.0f, 300.0f, 0.5f, 1.0f, 0xff0000ff },   // left   blue
    };
    if (FAILED(IDirect3DDevice9_CreateVertexBuffer(g_dev, sizeof(verts), 0,
            D3DFVF_CUSTOMVERTEX, D3DPOOL_DEFAULT, &g_vb, NULL))) {
        dbg("d3dtri: CreateVertexBuffer FAILED\n"); return 0;
    }
    void* p = NULL;
    if (FAILED(IDirect3DVertexBuffer9_Lock(g_vb, 0, sizeof(verts), &p, 0))) {
        dbg("d3dtri: VB Lock FAILED\n"); return 0;
    }
    CopyMemory(p, verts, sizeof(verts));
    IDirect3DVertexBuffer9_Unlock(g_vb);

    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_NONE);
    return 1;
}

static int g_frame = 0;
static void render(void) {
    if (!g_dev) return;
    int first = (g_frame < 3);
    if (first) dbg("d3dtri: render: Clear...\n");
    IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
        D3DCOLOR_XRGB(0, 40, 100), 1.0f, 0);   // dark blue background
    if (first) dbg("d3dtri: render: BeginScene...\n");
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(g_dev))) {
        IDirect3DDevice9_SetStreamSource(g_dev, 0, g_vb, 0, sizeof(CUSTOMVERTEX));
        IDirect3DDevice9_SetFVF(g_dev, D3DFVF_CUSTOMVERTEX);
        if (first) dbg("d3dtri: render: DrawPrimitive...\n");
        IDirect3DDevice9_DrawPrimitive(g_dev, D3DPT_TRIANGLELIST, 0, 1);
        if (first) dbg("d3dtri: render: EndScene...\n");
        IDirect3DDevice9_EndScene(g_dev);
    }
    if (first) dbg("d3dtri: render: Present...\n");
    IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
    if (first) dbg("d3dtri: render: Present DONE\n");
    g_frame++;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    dbg("d3dtri: WinMain start\n");
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "d3dtri";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3dtri", "Boxedwine64 D3D9 first light",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, 480, 360,
                              0, 0, hInst, 0);
    dbg(hwnd ? "d3dtri: window created\n" : "d3dtri: CreateWindow FAILED\n");
    int ok = initD3D(hwnd);
    if (!ok) dbg("d3dtri: initD3D failed; idling so the trace can be read\n");
    else     dbg("d3dtri: init OK, entering render loop\n");
    MSG msg; int frames = 0;
    for (;;) {
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        render();
        if (ok && (frames % 60 == 0) && frames < 600) {
            char b[64]; wsprintfA(b, "d3dtri: frame %d presented\n", frames); dbg(b);
        }
        if (++frames > 100000) break;
        Sleep(16);
    }
done:
    if (g_vb)  IDirect3DVertexBuffer9_Release(g_vb);
    if (g_dev) IDirect3DDevice9_Release(g_dev);
    if (g_d3d) IDirect3D9_Release(g_d3d);
    return 0;
}
