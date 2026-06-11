/* clipget.exe — write the win32 clipboard's CF_UNICODETEXT contents to
 * Z:\home\username\.bw64clip.out as UTF-8, then exit.
 *
 * The other half of the browser<->guest clipboard bridge (M2, see clipset.c).
 * The browser spawns this helper after the user clicks "Copy from app", polls
 * the output file in MEMFS, and puts the text on the system clipboard. Writes
 * the file even when the clipboard is empty (zero bytes) so the browser's poll
 * terminates either way.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hi; (void)hp; (void)cmd; (void)show;
    char *out = NULL;
    int outLen = 0;

    if (OpenClipboard(NULL))
    {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h)
        {
            const WCHAR *w = (const WCHAR *)GlobalLock(h);
            if (w)
            {
                outLen = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
                if (outLen > 0)
                {
                    out = (char *)malloc((size_t)outLen);
                    if (out)
                        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outLen, NULL, NULL);
                    outLen -= 1;      /* drop the NUL */
                }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }

    /* Write to a temp name then rename, so the browser's poll never reads a
     * half-written file. */
    FILE *f = fopen("Z:\\home\\username\\.bw64clip.tmp", "wb");
    if (!f) { free(out); return 1; }
    if (out && outLen > 0) fwrite(out, 1, (size_t)outLen, f);
    fclose(f);
    free(out);
    remove("Z:\\home\\username\\.bw64clip.out");
    rename("Z:\\home\\username\\.bw64clip.tmp", "Z:\\home\\username\\.bw64clip.out");
    return 0;
}
