/* hello32gui.exe — 32-bit (i686) Win32 GUI probe for M22 (32-bit exes under
 * the 64-bit wine session). Opens a window whose client area is painted solid
 * orange with a caption, so a headless browser test can detect it by pixels.
 * Build: i686-w64-mingw32-gcc -O2 -mwindows -o hello32gui.exe hello32gui.c */
#include <windows.h>

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(RGB(255, 128, 0));
        FillRect(dc, &rc, b);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        DrawTextA(dc, sizeof(void*) == 4 ? "Hello from a 32-bit exe" : "64-bit?!", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc = {0};
    MSG msg;
    (void)prev; (void)cmd;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "Hello32";
    RegisterClassA(&wc);
    HWND hw = CreateWindowExA(0, "Hello32", "hello32gui", WS_OVERLAPPEDWINDOW,
                              40, 40, 420, 260, NULL, NULL, inst, NULL);
    ShowWindow(hw, show ? show : SW_SHOW);
    UpdateWindow(hw);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
