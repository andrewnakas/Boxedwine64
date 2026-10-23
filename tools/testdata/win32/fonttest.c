/* fonttest.exe — M26 core-font probe: for Arial / Times New Roman / Courier New
 * create a 28px font, report the face GDI actually selected (GetTextFace) and
 * the extent of a sample string, and draw the sample in that font.
 * Build: i686-w64-mingw32-gcc -O2 -mwindows -o fonttest.exe fonttest.c */
#include <windows.h>
#include <stdio.h>

static const char* FACES[] = {"Arial", "Times New Roman", "Courier New"};
static const char* SAMPLE = "The quick brown fox 0123456789";

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; RECT rc;
        HDC dc = BeginPaint(h, &ps);
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        for (int i = 0; i < 3; i++) {
            HFONT f = CreateFontA(-28, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                  ANTIALIASED_QUALITY, 0, FACES[i]);
            HFONT old = (HFONT)SelectObject(dc, f);
            char face[64] = {0}, line[256];
            SIZE sz;
            GetTextFaceA(dc, sizeof(face), face);
            GetTextExtentPoint32A(dc, SAMPLE, lstrlenA(SAMPLE), &sz);
            TextOutA(dc, 10, 10 + i * 90, SAMPLE, lstrlenA(SAMPLE));
            SelectObject(dc, old);
            DeleteObject(f);
            snprintf(line, sizeof(line), "%s -> %s  (extent %ldx%ld)", FACES[i], face, sz.cx, sz.cy);
            TextOutA(dc, 10, 50 + i * 90, line, lstrlenA(line));
        }
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = "FontTest";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    HWND hw = CreateWindowExA(0, "FontTest", "fonttest", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              20, 20, 640, 300, NULL, NULL, inst, NULL);
    UpdateWindow(hw);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}
