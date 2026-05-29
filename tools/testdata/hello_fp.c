/* Real-clang FP probe: scalar double + packed float work, no libc.
 * Targets the opcode shapes glibc startup uses for math (ADDSD/MULSD/
 * CVTSI2SD/CVTSD2SI/UCOMISD/MOVSD/MOVAPD plus the FP-spill MOVDQU/A
 * pairs the register allocator emits to save XMM around calls).
 *
 * Static-syscall-only — same shape as hello_real.c, hello_wide.c. */

static long sys_write(int fd, const char* buf, long len) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "0"(1), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return ret;
}

static void sys_exit(int status) __attribute__((noreturn));
static void sys_exit(int status) {
    __asm__ volatile ("syscall" : : "a"(60), "D"((long)status)
                      : "rcx", "r11", "memory");
    __builtin_unreachable();
}

/* Force XMM register pressure: many doubles, function calls between
 * them. Clang's register allocator will spill via MOVSD m64, xmm and
 * reload via MOVSD xmm, m64 (F2 0F 11 / F2 0F 10). */
__attribute__((noinline))
static double mul_then_add(double a, double b, double c) {
    return a * b + c;            /* MULSD + ADDSD or VFMADD if FMA */
}

__attribute__((noinline))
static double poly(double x) {
    /* Horner: x^3 - 2x^2 + 3x - 4 */
    double a = mul_then_add(x, x, -2.0);     /* x*x - 2 */
    double b = mul_then_add(a, x, 3.0);      /* a*x + 3 */
    return mul_then_add(b, x, -4.0);          /* b*x - 4 */
}

__attribute__((noinline))
static int compare_doubles(double a, double b) {
    /* UCOMISD + branch sequence. */
    if (a < b) return -1;
    if (a > b) return  1;
    return 0;
}

void _start(void) {
    static const char msg[] = "hello from fp-shape probe\n";
    sys_write(1, msg, sizeof(msg) - 1);

    double x = 2.5;
    double y = poly(x);                     /* 2.5^3 - 2*2.5^2 + 3*2.5 - 4
                                               = 15.625 - 12.5 + 7.5 - 4 = 6.625 */
    int    cmp = compare_doubles(y, 6.0);    /*  1 */
    int    cmp2 = compare_doubles(y, y);     /*  0 */
    long   cast = (long)(y * 10.0);         /* 66 (CVTTSD2SI) */

    /* Combine, mask to U8, exit. cast=66, cmp=1, cmp2=0 → 67 */
    long status = (cast + cmp + cmp2) & 0xFF;
    sys_exit((int)status);
}
