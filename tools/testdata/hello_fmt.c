/* Float-formatting probe — the operations glibc's __printf_fp / dtoa hit
 * when turning a double into a decimal string, exercised without libc.
 * Converts 3.14159 into "3.14159\n" by repeated extract-int-digit /
 * multiply-by-10 on the fractional part, writing each digit. This is the
 * Milestone C exit-criterion shape ("printf formatting floats works")
 * reduced to the arithmetic kernel: scalar double mul/sub, double->int
 * truncation (CVTTSD2SI), int->double (CVTSI2SD).
 *
 * Exit status = number of fractional digits emitted (6), so the smoke
 * harness has a deterministic assert; the real proof is the decimal
 * string tee'd to host stdout. NOTE: 3.14159 is not exactly representable
 * as a double, so naive digit extraction drifts in the last place
 * (emits ...58/...9 rather than a clean ...59) — that's correct IEEE-754
 * behaviour on real x86 too, not an emulator artifact. The point is that
 * MULSD/SUBSD/CVTTSD2SI/CVTSI2SD all execute and compose correctly.
 *
 * No libc. */

static long sys_write(int fd, const char* buf, long len) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret)
        : "0"(1), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return ret;
}

static void sys_exit(int status) __attribute__((noreturn));
static void sys_exit(int status) {
    __asm__ volatile ("syscall" : : "a"(60), "D"((long)status)
                      : "rcx", "r11", "memory");
    __builtin_unreachable();
}

static void putc1(char c) { sys_write(1, &c, 1); }

/* noinline + volatile to keep clang from constant-folding the whole
 * conversion into a precomputed string at -O2. */
static volatile double g_value = 3.14159;

static __attribute__((noinline)) long trunc_to_int(double d) {
    return (long)d;                 /* CVTTSD2SI */
}
static __attribute__((noinline)) double int_to_double(long x) {
    return (double)x;               /* CVTSI2SD */
}

void _start(void) {
    static const char msg[] = "fmt probe: double -> decimal string\n";
    sys_write(1, msg, sizeof(msg) - 1);

    double v = g_value;

    /* Integer part. */
    long ip = trunc_to_int(v);                  /* 3 */
    putc1((char)('0' + (ip % 10)));
    putc1('.');

    /* Fractional part: subtract int, then for each digit multiply by 10,
     * take the integer part, subtract it, repeat. */
    double frac = v - int_to_double(ip);        /* 0.14159 */
    int digits = 0;
    for (int i = 0; i < 6; i++) {
        frac = frac * 10.0;                     /* MULSD */
        long d = trunc_to_int(frac);            /* CVTTSD2SI */
        if (d < 0) d = 0;
        if (d > 9) d = 9;
        putc1((char)('0' + d));
        frac = frac - int_to_double(d);         /* SUBSD */
        digits++;
    }
    putc1('\n');

    sys_exit(digits);                           /* 6 */
}
