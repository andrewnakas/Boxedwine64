/* clipset.exe — put the contents of Z:\home\username\.bw64clip.txt (UTF-8)
 * onto the win32 clipboard as CF_UNICODETEXT, then exit.
 *
 * Half of the browser<->guest clipboard bridge (M2). The browser writes the
 * file into the emulator's MEMFS and spawns this helper into the running wine
 * session; the next paste in any wine app reads the text straight from the
 * wineserver-managed clipboard. This bypasses X selections entirely — the
 * X-side clipboard manager (explorer's clipboard thread bridging X<->win32)
 * is not functional under the minimal XWire server, but the pure win32
 * clipboard path through wineserver is.
 *
 * Build (see build-games.sh): x86_64-w64-mingw32-gcc -O2 -municode? no — plain
 * console-less main; -mwindows keeps it from needing a console.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hi; (void)hp; (void)cmd; (void)show;
    FILE *f = fopen("Z:\\home\\username\\.bw64clip.txt", "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return 1; }
    if (len > 8 * 1024 * 1024) len = 8 * 1024 * 1024;   /* sanity cap */
    char *buf = (char *)malloc(len ? (size_t)len : 1);
    if (!buf) { fclose(f); return 1; }
    long got = (long)fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got < 0) got = 0;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, (int)got, NULL, 0);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, ((SIZE_T)wlen + 1) * sizeof(WCHAR));
    if (!h) { free(buf); return 1; }
    WCHAR *w = (WCHAR *)GlobalLock(h);
    MultiByteToWideChar(CP_UTF8, 0, buf, (int)got, w, wlen);
    w[wlen] = 0;
    GlobalUnlock(h);
    free(buf);

    if (!OpenClipboard(NULL)) { GlobalFree(h); return 2; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, h)) { CloseClipboard(); GlobalFree(h); return 3; }
    CloseClipboard();                 /* clipboard owns h now */
    return 0;
}
