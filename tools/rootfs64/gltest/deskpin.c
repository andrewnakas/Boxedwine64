/* deskpin.exe — Boxedwine boot-reliability helper (M18).
 *
 * wineserver closes an "empty" desktop (only explorer's own threads left as
 * users) after ONE second (server/winstation.c close_desktop_timeout). Under
 * Boxedwine's interpreted CPU the wineboot-chain -> app handoff routinely
 * leaves the desktop empty for many seconds, so explorer /desktop got
 * WM_CLOSE mid-boot and the late-arriving app found no desktop (~1-in-3 cold
 * boots died this way; explorer exiting cleanly ~51s in was the signature).
 *
 * This helper simply holds a hidden top-level window on the Default desktop
 * and blocks in GetMessage forever — it is a desktop *user*, so the desktop's
 * user count never drops to explorer-only and the close timeout never arms.
 * Zero CPU cost (blocked in a wait), all wine binaries stay stock.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -mwindows -o deskpin.exe deskpin.c
 * The launcher spawns it right after the session comes up (wine64-launcher.js).
 */
#include <windows.h>

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* Never participate in shutdown-by-close; only an explicit kill ends us. */
    if (m == WM_CLOSE) return 0;
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc = {0};
    MSG msg;

    (void)prev; (void)cmd; (void)show;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = "BoxedwineDeskPin";
    if (!RegisterClassA(&wc)) return 1;
    /* Hidden (never shown, never mapped) — just a desktop user. */
    if (!CreateWindowExA(0, wc.lpszClassName, "deskpin", WS_OVERLAPPED,
                         0, 0, 1, 1, NULL, NULL, inst, NULL))
        return 1;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
