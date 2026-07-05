/* soundtest.exe — M5 audio-bridge probe (v2: waveOut with explicit MMRESULTs).
 * Build: x86_64-w64-mingw32-gcc -O2 -o soundtest.exe soundtest.c -lwinmm
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

int main(void)
{
    const int rate = 22050, n = 22050 * 2;
    static int16_t pcm[22050 * 2];
    WAVEFORMATEX wf;
    HWAVEOUT h = NULL;
    WAVEHDR hdr;
    MMRESULT r;
    int i;

    printf("soundtest: waveOutGetNumDevs=%u\n", (unsigned)waveOutGetNumDevs());
    fflush(stdout);

    for (i = 0; i < n; i++)
        pcm[i] = (int16_t)(12000.0 * sin(2.0 * 3.14159265358979 * 440.0 * i / rate));

    ZeroMemory(&wf, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = rate * 2;

    r = waveOutOpen(&h, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
    printf("soundtest: waveOutOpen -> %u\n", (unsigned)r);
    fflush(stdout);
    if (r != MMSYSERR_NOERROR) return 1;

    ZeroMemory(&hdr, sizeof(hdr));
    hdr.lpData = (LPSTR)pcm;
    hdr.dwBufferLength = n * 2;
    r = waveOutPrepareHeader(h, &hdr, sizeof(hdr));
    printf("soundtest: prepare -> %u\n", (unsigned)r);
    r = waveOutWrite(h, &hdr, sizeof(hdr));
    printf("soundtest: write -> %u, waiting for playback...\n", (unsigned)r);
    fflush(stdout);
    {
        DWORD t0 = GetTickCount();
        while (!(hdr.dwFlags & WHDR_DONE) && GetTickCount() - t0 < 8000)
            Sleep(50);
        printf("soundtest: done=%d elapsed=%lums\n",
               (int)((hdr.dwFlags & WHDR_DONE) ? 1 : 0),
               (unsigned long)(GetTickCount() - t0));
    }
    waveOutUnprepareHeader(h, &hdr, sizeof(hdr));
    waveOutClose(h);
    fflush(stdout);
    return 0;
}
