/* ddrawtest.c -- M28 DirectDraw probe.
 *
 *   ddrawtest.exe       windowed: DDSCL_NORMAL primary + clipper, an offscreen
 *                       surface filled by Lock (gradient) and Blt COLORFILL,
 *                       blitted to the window every frame
 *   ddrawtest.exe 8     exclusive fullscreen 640x480x8: palette with color bars,
 *                       primary + back buffer, Lock-written indices, Flip
 *
 * Every step prints "ddrawtest: <step> hr=0x..." to stderr so a browser console
 * probe can see how far it got; "ddrawtest: frame N" proves presenting works.
 *
 * Build: i686-w64-mingw32-gcc -O2 -o ddrawtest.exe ddrawtest.c -lddraw -ldxguid -lgdi32 -mwindows
 */
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>

static HWND g_hwnd;
static int g_mode8;

static void say(const char *step, HRESULT hr)
{
    fprintf(stderr, "ddrawtest: %s hr=0x%08lx\n", step, (unsigned long)hr);
    fflush(stderr);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { DestroyWindow(h); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static int pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return 0;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 1;
}

static int run_windowed(IDirectDraw7 *dd)
{
    HRESULT hr;
    DDSURFACEDESC2 sd;
    IDirectDrawSurface7 *prim = NULL, *off = NULL;
    IDirectDrawClipper *clip = NULL;
    const int W = 256, H = 256;
    int frame;

    hr = IDirectDraw7_SetCooperativeLevel(dd, g_hwnd, DDSCL_NORMAL); say("SetCooperativeLevel(NORMAL)", hr);
    if (FAILED(hr)) return 1;

    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.dwFlags = DDSD_CAPS;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    hr = IDirectDraw7_CreateSurface(dd, &sd, &prim, NULL); say("CreateSurface(primary)", hr);
    if (FAILED(hr)) return 1;

    hr = IDirectDraw7_CreateClipper(dd, 0, &clip, NULL); say("CreateClipper", hr);
    if (SUCCEEDED(hr)) {
        IDirectDrawClipper_SetHWnd(clip, 0, g_hwnd);
        hr = IDirectDrawSurface7_SetClipper(prim, clip); say("SetClipper", hr);
    }

    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    sd.dwWidth = W;
    sd.dwHeight = H;
    sd.ddpfPixelFormat.dwSize = sizeof sd.ddpfPixelFormat;
    sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
    sd.ddpfPixelFormat.dwRGBBitCount = 32;
    sd.ddpfPixelFormat.dwRBitMask = 0xff0000;
    sd.ddpfPixelFormat.dwGBitMask = 0x00ff00;
    sd.ddpfPixelFormat.dwBBitMask = 0x0000ff;
    hr = IDirectDraw7_CreateSurface(dd, &sd, &off, NULL); say("CreateSurface(offscreen 32bpp)", hr);
    if (FAILED(hr)) return 1;

    for (frame = 0; pump(); frame++) {
        DDBLTFX fx;
        RECT r, box;
        POINT pt = {0, 0};
        int x, y;

        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        hr = IDirectDrawSurface7_Lock(off, NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
        if (frame == 0) say("Lock(offscreen)", hr);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < H; y++) {
                DWORD *row = (DWORD *)((BYTE *)sd.lpSurface + y * sd.lPitch);
                for (x = 0; x < W; x++)
                    row[x] = (x << 16) | (y << 8) | ((frame * 4) & 0xff);
            }
            IDirectDrawSurface7_Unlock(off, NULL);
        }
        /* a white box that moves: exercises Blt COLORFILL with a dest rect */
        memset(&fx, 0, sizeof fx);
        fx.dwSize = sizeof fx;
        fx.dwFillColor = 0xffffff;
        box.left = (frame * 2) % (W - 32); box.top = 112; box.right = box.left + 32; box.bottom = 144;
        hr = IDirectDrawSurface7_Blt(off, &box, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        if (frame == 0) say("Blt(COLORFILL)", hr);

        GetClientRect(g_hwnd, &r);
        ClientToScreen(g_hwnd, &pt);
        OffsetRect(&r, pt.x, pt.y);
        hr = IDirectDrawSurface7_Blt(prim, &r, off, NULL, DDBLT_WAIT, NULL);
        if (frame == 0) say("Blt(offscreen->primary)", hr);
        if (frame % 60 == 0) { fprintf(stderr, "ddrawtest: frame %d\n", frame); fflush(stderr); }
        Sleep(16);
    }
    return 0;
}

static int run_8bpp(IDirectDraw7 *dd)
{
    static const DWORD bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                                   0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
    HRESULT hr;
    DDSURFACEDESC2 sd;
    DDSCAPS2 caps;
    IDirectDrawSurface7 *prim = NULL, *back = NULL;
    IDirectDrawPalette *pal = NULL;
    PALETTEENTRY pe[256];
    int i, frame;

    hr = IDirectDraw7_SetCooperativeLevel(dd, g_hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN); say("SetCooperativeLevel(EXCLUSIVE)", hr);
    if (FAILED(hr)) return 1;
    hr = IDirectDraw7_SetDisplayMode(dd, 640, 480, 8, 0, 0); say("SetDisplayMode(640x480x8)", hr);
    if (FAILED(hr)) return 1;

    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    sd.dwBackBufferCount = 1;
    hr = IDirectDraw7_CreateSurface(dd, &sd, &prim, NULL); say("CreateSurface(flip chain)", hr);
    if (FAILED(hr)) return 1;
    memset(&caps, 0, sizeof caps);
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    hr = IDirectDrawSurface7_GetAttachedSurface(prim, &caps, &back); say("GetAttachedSurface(back)", hr);
    if (FAILED(hr)) return 1;

    /* entries 0-7: color bars; 8-255: a grey ramp */
    for (i = 0; i < 256; i++) {
        DWORD c = i < 8 ? bars[i] : (DWORD)(i * 0x010101);
        pe[i].peRed = (BYTE)(c >> 16); pe[i].peGreen = (BYTE)(c >> 8); pe[i].peBlue = (BYTE)c; pe[i].peFlags = 0;
    }
    hr = IDirectDraw7_CreatePalette(dd, DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, NULL); say("CreatePalette", hr);
    if (FAILED(hr)) return 1;
    hr = IDirectDrawSurface7_SetPalette(prim, pal); say("SetPalette", hr);

    for (frame = 0; pump(); frame++) {
        int x, y;
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        hr = IDirectDrawSurface7_Lock(back, NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
        if (frame == 0) say("Lock(back 8bpp)", hr);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < 480; y++) {
                BYTE *row = (BYTE *)sd.lpSurface + y * sd.lPitch;
                for (x = 0; x < 640; x++)
                    row[x] = y < 320 ? (BYTE)(x / 80) : (BYTE)(8 + ((x + frame * 2) % 640) * 247 / 640);
            }
            IDirectDrawSurface7_Unlock(back, NULL);
        }
        hr = IDirectDrawSurface7_Flip(prim, NULL, DDFLIP_WAIT);
        if (frame == 0) say("Flip", hr);
        if (frame % 60 == 0) { fprintf(stderr, "ddrawtest: frame %d\n", frame); fflush(stderr); }
        Sleep(16);
    }
    IDirectDraw7_RestoreDisplayMode(dd);
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc = {0};
    IDirectDraw7 *dd = NULL;
    HRESULT hr;

    g_mode8 = cmd && cmd[0] == '8';
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ddrawtest";
    RegisterClassA(&wc);
    if (g_mode8)
        g_hwnd = CreateWindowExA(WS_EX_TOPMOST, "ddrawtest", "ddrawtest 8bpp", WS_POPUP | WS_VISIBLE,
                                 0, 0, 640, 480, NULL, NULL, inst, NULL);
    else
        g_hwnd = CreateWindowA("ddrawtest", "ddrawtest", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               40, 40, 512 + 16, 384 + 39, NULL, NULL, inst, NULL);

    hr = DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL); say("DirectDrawCreateEx", hr);
    if (FAILED(hr)) return 1;
    return g_mode8 ? run_8bpp(dd) : run_windowed(dd);
}
