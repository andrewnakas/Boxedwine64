/* modeswitch.exe — M27 display-mode probe. Lists display modes, switches to
 * 640x480 with ChangeDisplaySettings(CDS_FULLSCREEN), then opens a 640x480
 * fullscreen popup: GREEN with the mode count when the switch succeeded, RED
 * with the error code when it failed (pixel-detectable in a headless test).
 * Build: i686-w64-mingw32-gcc -O2 -mwindows -o modeswitch.exe modeswitch.c */
#include <windows.h>
#include <stdio.h>

static char g_text[256];
static LONG g_result;

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; RECT rc;
        HDC dc = BeginPaint(h, &ps);
        GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(g_result == DISP_CHANGE_SUCCESSFUL ? RGB(0, 170, 0) : RGB(200, 0, 0));
        FillRect(dc, &rc, b);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextA(dc, g_text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { DestroyWindow(h); return 0; }
    if (m == WM_DESTROY) { ChangeDisplaySettingsA(NULL, 0); PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    DEVMODEA dm; int modes = 0;
    (void)prev; (void)cmd; (void)show;
    memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
    while (EnumDisplaySettingsA(NULL, modes, &dm)) modes++;
    memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = 640; dm.dmPelsHeight = 480;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    g_result = ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN);
    DEVMODEA now; memset(&now, 0, sizeof(now)); now.dmSize = sizeof(now);
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &now);
    snprintf(g_text, sizeof(g_text), "ChangeDisplaySettings 640x480 -> %ld; now %lux%lu; %d modes",
             g_result, now.dmPelsWidth, now.dmPelsHeight, modes);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = "ModeSwitch";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    HWND hw = CreateWindowExA(WS_EX_TOPMOST, "ModeSwitch", "modeswitch", WS_POPUP | WS_VISIBLE,
                              0, 0, 640, 480, NULL, NULL, inst, NULL);
    UpdateWindow(hw);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}
