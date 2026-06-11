// Minimal /dev/dsp tone generator (native Linux x86_64 ELF, no libc audio).
// Proves the Boxedwine OSS sink -> KDspAudio -> WebAudio path is audible.
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

// OSS ioctls (from <sys/soundcard.h>) — hardcoded so we need no headers.
#define SNDCTL_DSP_SPEED    0xc0045002
#define SNDCTL_DSP_SETFMT   0xc0045005
#define SNDCTL_DSP_CHANNELS 0xc0045006
#define AFMT_S16_LE         0x00000010

int main(void) {
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) { const char* m = "open /dev/dsp failed\n"; write(2, m, 21); return 1; }
    int fmt = AFMT_S16_LE, ch = 1, rate = 22050;
    ioctl(fd, SNDCTL_DSP_SETFMT, &fmt);
    ioctl(fd, SNDCTL_DSP_CHANNELS, &ch);
    ioctl(fd, SNDCTL_DSP_SPEED, &rate);

    // ~2 seconds of a 440Hz square-ish sine approximated with a triangle.
    const int N = 22050 * 2;
    static int16_t buf[4410];   // 0.2s chunks
    int phase = 0, period = rate / 440;
    int written = 0;
    while (written < N) {
        int n = sizeof(buf)/sizeof(buf[0]);
        if (written + n > N) n = N - written;
        for (int i = 0; i < n; i++) {
            int p = phase % period;
            // triangle wave -> smoother than square, clearly tonal
            int t = (p < period/2) ? p : (period - p);
            buf[i] = (int16_t)((t - period/4) * 600);
            phase++;
        }
        write(fd, buf, n * sizeof(int16_t));
        written += n;
    }
    const char* m = "tone done\n"; write(1, m, 10);
    close(fd);
    return 0;
}
