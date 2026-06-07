/* snake.c — a tiny, self-contained Snake for Boxedwine64's in-browser wine64.
 *
 * Pure Win32 + GDI (the proven-working render path: RegisterClass / CreateWindow
 * / PeekMessage loop / double-buffered BitBlt — same as notepad and winemine).
 * No external assets, no DirectX, no OpenGL. Cross-compiled to a 64-bit Windows
 * PE with mingw-w64. Arrow keys / WASD to steer, eat the red food, don't hit the
 * wall or yourself. Space restarts after game over.
 */
#include <windows.h>

#define CELL   18
#define COLS   28
#define ROWS   22
#define W      (COLS*CELL)
#define H      (ROWS*CELL)
#define MAXLEN (COLS*ROWS)

static int sx[MAXLEN], sy[MAXLEN]; /* snake body cells, [0] = head */
static int slen;
static int dx, dy;                 /* current direction */
static int fx, fy;                 /* food cell */
static int dead, score;
static unsigned rngState = 0x1234abcd;

static unsigned rnd(void) {        /* xorshift — no CRT rand dependency */
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}

static void placeFood(void) {
    for (;;) {
        int x = rnd() % COLS, y = rnd() % ROWS, i, hit = 0;
        for (i = 0; i < slen; i++) if (sx[i] == x && sy[i] == y) { hit = 1; break; }
        if (!hit) { fx = x; fy = y; return; }
    }
}

static void reset(void) {
    slen = 4;
    sx[0] = COLS / 2;     sy[0] = ROWS / 2;
    sx[1] = COLS / 2 - 1; sy[1] = ROWS / 2;
    sx[2] = COLS / 2 - 2; sy[2] = ROWS / 2;
    sx[3] = COLS / 2 - 3; sy[3] = ROWS / 2;
    dx = 1; dy = 0;
    dead = 0; score = 0;
    placeFood();
}

static void step(void) {
    int nx, ny, i;
    if (dead) return;
    nx = sx[0] + dx; ny = sy[0] + dy;
    /* wall collision */
    if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS) { dead = 1; return; }
    /* self collision (skip the tail cell, which will move away unless we grow) */
    for (i = 0; i < slen - 1; i++) if (sx[i] == nx && sy[i] == ny) { dead = 1; return; }
    if (nx == fx && ny == fy) {
        /* grow: shift body, keep tail */
        for (i = slen; i > 0; i--) { sx[i] = sx[i-1]; sy[i] = sy[i-1]; }
        sx[0] = nx; sy[0] = ny;
        if (slen < MAXLEN) slen++;
        score += 10;
        placeFood();
    } else {
        for (i = slen - 1; i > 0; i--) { sx[i] = sx[i-1]; sy[i] = sy[i-1]; }
        sx[0] = nx; sy[0] = ny;
    }
}

static void fillCell(HDC dc, int cx, int cy, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    RECT r; r.left = cx*CELL+1; r.top = cy*CELL+1; r.right = cx*CELL+CELL-1; r.bottom = cy*CELL+CELL-1;
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void paint(HDC dc) {
    RECT all = { 0, 0, W, H };
    HBRUSH bg = CreateSolidBrush(RGB(18, 20, 24));
    int i;
    char buf[64]; int n = 0;
    const char *msg;
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    /* food */
    fillCell(dc, fx, fy, RGB(220, 60, 60));
    /* snake: head brighter than body */
    for (i = 0; i < slen; i++)
        fillCell(dc, sx[i], sy[i], i == 0 ? RGB(140, 230, 120) : RGB(90, 180, 90));
    /* score */
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(200, 200, 200));
    {
        const char *p = "Score: ";
        int v = score, started = 0, d;
        while (*p) buf[n++] = *p++;
        if (v == 0) buf[n++] = '0';
        else { char tmp[16]; int t = 0; while (v) { tmp[t++] = '0' + v % 10; v /= 10; }
               while (t) buf[n++] = tmp[--t]; }
        (void)started; (void)d;
        TextOutA(dc, 6, 4, buf, n);
    }
    if (dead) {
        msg = "GAME OVER — press Space";
        SetTextColor(dc, RGB(240, 200, 120));
        TextOutA(dc, W/2 - 90, H/2 - 8, msg, lstrlenA(msg));
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT:  case 'A': if (dx != 1)  { dx = -1; dy = 0; } break;
        case VK_RIGHT: case 'D': if (dx != -1) { dx = 1;  dy = 0; } break;
        case VK_UP:    case 'W': if (dy != 1)  { dx = 0;  dy = -1; } break;
        case VK_DOWN:  case 'S': if (dy != -1) { dx = 0;  dy = 1; } break;
        case VK_SPACE: if (dead) reset(); break;
        case VK_ESCAPE: PostQuitMessage(0); break;
        }
        return 0;
    case WM_TIMER:
        step();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        /* double-buffer to avoid flicker */
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        paint(mem);
        BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;
    RECT r;
    (void)hPrev; (void)cmd;

    reset();

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "bw64snake";
    RegisterClassA(&wc);

    /* size the window so the client area is exactly W x H */
    r.left = 0; r.top = 0; r.right = W; r.bottom = H;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA("bw64snake", "Snake — Boxedwine64",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top,
                         NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    SetTimer(hwnd, 1, 110, NULL); /* game tick */

    /* PeekMessage loop with a small Sleep — the idiom proven to render under the
     * in-browser emulator (see gltest/msgloop.c / glcube.c). */
    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(10);
    }
}
