/* tetris.c — a tiny self-contained Tetris for Boxedwine64's in-browser wine64.
 *
 * Pure Win32 + GDI, double-buffered (the proven render path, same as snake.c /
 * notepad / winemine). No external assets. mingw-w64 -> 64-bit Windows PE.
 * Left/Right or A/D move, Up or W rotate, Down or S soft-drop, Space hard-drop,
 * Esc quit, Space restarts after game over.
 */
#include <windows.h>

#define COLS  10
#define ROWS  20
#define CELL  22
#define MARGIN 14
#define BOARD_W (COLS*CELL)
#define BOARD_H (ROWS*CELL)
#define W (BOARD_W + MARGIN*2 + 120)
#define H (BOARD_H + MARGIN*2)

/* 7 tetrominoes, 4 rotations each, as 4x4 bitmasks (row-major, bit 0 = col 0). */
static const unsigned short PIECES[7][4] = {
    { 0x0F00, 0x2222, 0x00F0, 0x4444 }, /* I */
    { 0x8E00, 0x6440, 0x0E20, 0x44C0 }, /* J */
    { 0x2E00, 0x4460, 0x0E80, 0xC440 }, /* L */
    { 0x6600, 0x6600, 0x6600, 0x6600 }, /* O */
    { 0x6C00, 0x4620, 0x06C0, 0x8C40 }, /* S */
    { 0x4E00, 0x4640, 0x0E40, 0x4C40 }, /* T */
    { 0xC600, 0x2640, 0x0C60, 0x4C80 }, /* Z */
};
static const COLORREF COLORS[7] = {
    RGB(80,200,220), RGB(80,110,220), RGB(230,160,60),
    RGB(230,220,70), RGB(90,210,110), RGB(190,100,210), RGB(230,90,90),
};

static int board[ROWS][COLS]; /* 0 empty, else piece index+1 */
static int curP, curR, curX, curY;
static int nextP;
static int dead, score, lines;
static unsigned rngState = 0xC0FFEE11;

static unsigned rnd(void) {
    rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
    return rngState;
}

static int cellOf(int p, int r, int x, int y) { /* is (x,y) within the 4x4 set? */
    if (x < 0 || y < 0 || x > 3 || y > 3) return 0;
    return (PIECES[p][r] >> (y*4 + x)) & 1;
}

static int collides(int p, int r, int ox, int oy) {
    int x, y;
    for (y = 0; y < 4; y++) for (x = 0; x < 4; x++) {
        if (!cellOf(p, r, x, y)) continue;
        int bx = ox + x, by = oy + y;
        if (bx < 0 || bx >= COLS || by >= ROWS) return 1;
        if (by >= 0 && board[by][bx]) return 1;
    }
    return 0;
}

static void spawn(void) {
    curP = nextP; nextP = rnd() % 7;
    curR = 0; curX = 3; curY = -1;
    if (collides(curP, curR, curX, curY)) dead = 1;
}

static void reset(void) {
    int x, y;
    for (y = 0; y < ROWS; y++) for (x = 0; x < COLS; x++) board[y][x] = 0;
    dead = 0; score = 0; lines = 0;
    nextP = rnd() % 7;
    spawn();
}

static void lockPiece(void) {
    int x, y, cleared = 0;
    for (y = 0; y < 4; y++) for (x = 0; x < 4; x++)
        if (cellOf(curP, curR, x, y)) {
            int by = curY + y, bx = curX + x;
            if (by >= 0 && by < ROWS && bx >= 0 && bx < COLS) board[by][bx] = curP + 1;
        }
    /* clear full rows */
    for (y = ROWS - 1; y >= 0; y--) {
        int full = 1, c;
        for (c = 0; c < COLS; c++) if (!board[y][c]) { full = 0; break; }
        if (full) {
            int yy;
            for (yy = y; yy > 0; yy--) for (c = 0; c < COLS; c++) board[yy][c] = board[yy-1][c];
            for (c = 0; c < COLS; c++) board[0][c] = 0;
            cleared++; y++; /* recheck this row */
        }
    }
    if (cleared) { lines += cleared; score += (cleared*cleared)*100; }
    spawn();
}

static void softDrop(void) {
    if (dead) return;
    if (!collides(curP, curR, curX, curY + 1)) curY++;
    else lockPiece();
}

static void hardDrop(void) {
    if (dead) return;
    while (!collides(curP, curR, curX, curY + 1)) { curY++; score += 2; }
    lockPiece();
}

static void rotate(void) {
    int nr = (curR + 1) & 3;
    if (!collides(curP, nr, curX, curY)) curR = nr;
    else if (!collides(curP, nr, curX - 1, curY)) { curX--; curR = nr; }
    else if (!collides(curP, nr, curX + 1, curY)) { curX++; curR = nr; }
}

static void move(int d) { if (!dead && !collides(curP, curR, curX + d, curY)) curX += d; }

static void drawCell(HDC dc, int px, int py, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    RECT r; r.left = px+1; r.top = py+1; r.right = px+CELL-1; r.bottom = py+CELL-1;
    FillRect(dc, &r, b); DeleteObject(b);
}

static void numAt(HDC dc, int x, int y, const char *label, int v) {
    char buf[48]; int n = 0; const char *p = label;
    while (*p) buf[n++] = *p++;
    { char t[16]; int k = 0; if (v == 0) t[k++] = '0'; while (v) { t[k++] = '0'+v%10; v/=10; }
      while (k) buf[n++] = t[--k]; }
    TextOutA(dc, x, y, buf, n);
}

static void paint(HDC dc) {
    RECT all = { 0, 0, W, H };
    HBRUSH bg = CreateSolidBrush(RGB(16, 18, 22));
    HBRUSH well = CreateSolidBrush(RGB(28, 30, 36));
    RECT wr = { MARGIN, MARGIN, MARGIN+BOARD_W, MARGIN+BOARD_H };
    int x, y;
    FillRect(dc, &all, bg); DeleteObject(bg);
    FillRect(dc, &wr, well); DeleteObject(well);
    /* settled blocks */
    for (y = 0; y < ROWS; y++) for (x = 0; x < COLS; x++)
        if (board[y][x]) drawCell(dc, MARGIN + x*CELL, MARGIN + y*CELL, COLORS[board[y][x]-1]);
    /* current piece */
    if (!dead) for (y = 0; y < 4; y++) for (x = 0; x < 4; x++)
        if (cellOf(curP, curR, x, y) && curY + y >= 0)
            drawCell(dc, MARGIN + (curX+x)*CELL, MARGIN + (curY+y)*CELL, COLORS[curP]);
    /* HUD */
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(210, 210, 210));
    {
        int hx = MARGIN*2 + BOARD_W;
        numAt(dc, hx, MARGIN, "Score: ", score);
        numAt(dc, hx, MARGIN + 22, "Lines: ", lines);
        TextOutA(dc, hx, MARGIN + 60, "Next:", 5);
        for (y = 0; y < 4; y++) for (x = 0; x < 4; x++)
            if (cellOf(nextP, 0, x, y))
                drawCell(dc, hx + x*CELL, MARGIN + 80 + y*CELL, COLORS[nextP]);
        SetTextColor(dc, RGB(150,150,150));
        TextOutA(dc, hx, H - 70, "Arrows move", 11);
        TextOutA(dc, hx, H - 54, "Up rotate", 9);
        TextOutA(dc, hx, H - 38, "Space drop", 10);
    }
    if (dead) {
        SetTextColor(dc, RGB(240, 200, 120));
        TextOutA(dc, MARGIN + 20, MARGIN + BOARD_H/2 - 8, "GAME OVER - Space", 17);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT:  case 'A': move(-1); break;
        case VK_RIGHT: case 'D': move(1);  break;
        case VK_UP:    case 'W': rotate();  break;
        case VK_DOWN:  case 'S': softDrop(); break;
        case VK_SPACE: if (dead) reset(); else hardDrop(); break;
        case VK_ESCAPE: PostQuitMessage(0); break;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_TIMER:
        softDrop();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        paint(mem);
        BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    WNDCLASSA wc; HWND hwnd; MSG msg; RECT r;
    (void)hPrev; (void)cmd;
    reset();
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "bw64tetris";
    RegisterClassA(&wc);
    r.left = 0; r.top = 0; r.right = W; r.bottom = H;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA("bw64tetris", "Tetris - Boxedwine64",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    SetTimer(hwnd, 1, 450, NULL);
    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        Sleep(10);
    }
}
