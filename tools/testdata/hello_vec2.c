/* Second vector probe — targets SSE3 FP-horizontal + SSE4.1 ops that the
 * first probe didn't reach. As before: each op is run via inline asm so
 * the optimizer can't lower it away; the exit value is not a semantic
 * assertion (the cpu64SelfTest cases carry correctness) — it just has to
 * be deterministic so the smoke harness can detect a regression, and the
 * run must complete without the unimpl-tracer firing.
 *
 * Ops exercised:
 *   HADDPS   F2-less, F2 0F 7C — horizontal add packed singles
 *   HADDPD   66 0F 7C          — horizontal add packed doubles
 *   MOVSHDUP F3 0F 16          — duplicate odd singles
 *   MOVSLDUP F3 0F 12          — duplicate even singles
 *   PMULLD   66 0F 38 40       — packed 32-bit multiply low (SSE4.1)
 *   PMINSD   66 0F 38 39       — packed signed dword min (SSE4.1)
 *
 * No libc. */

typedef float  v4sf __attribute__((vector_size(16)));
typedef double v2df __attribute__((vector_size(16)));
typedef int    v4si __attribute__((vector_size(16)));

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

static volatile int vol_two = 2;

static __attribute__((noinline)) v4sf do_haddps(v4sf a, v4sf b) {
    __asm__ volatile ("haddps %1, %0" : "+x"(a) : "x"(b));
    return a;
}
static __attribute__((noinline)) v2df do_haddpd(v2df a, v2df b) {
    __asm__ volatile ("haddpd %1, %0" : "+x"(a) : "x"(b));
    return a;
}
static __attribute__((noinline)) v4sf do_movshdup(v4sf a) {
    v4sf r;
    __asm__ volatile ("movshdup %1, %0" : "=x"(r) : "x"(a));
    return r;
}
static __attribute__((noinline)) v4sf do_movsldup(v4sf a) {
    v4sf r;
    __asm__ volatile ("movsldup %1, %0" : "=x"(r) : "x"(a));
    return r;
}
static __attribute__((noinline)) v4si do_pmulld(v4si a, v4si b) {
    __asm__ volatile ("pmulld %1, %0" : "+x"(a) : "x"(b));
    return a;
}
static __attribute__((noinline)) v4si do_pminsd(v4si a, v4si b) {
    __asm__ volatile ("pminsd %1, %0" : "+x"(a) : "x"(b));
    return a;
}

void _start(void) {
    static const char msg[] = "vec2 probe: SSE3 FP-horiz + SSE4.1\n";
    sys_write(1, msg, sizeof(msg) - 1);

    int acc = 0;

    /* HADDPS {1,2,3,4} {5,6,7,8} -> {1+2,3+4,5+6,7+8} = {3,7,11,15}. */
    v4sf a = { 1.0f, 2.0f, 3.0f, 4.0f };
    v4sf b = { 5.0f, 6.0f, 7.0f, 8.0f };
    v4sf hs = do_haddps(a, b);
    acc += (int)hs[0];                     /* 3 */

    /* HADDPD {1.0,2.0} {3.0,4.0} -> {1+2, 3+4} = {3,7}. */
    v2df c = { 1.0, 2.0 };
    v2df d = { 3.0, 4.0 };
    v2df hd = do_haddpd(c, d);
    acc += (int)hd[1];                     /* 7  -> acc 10 */

    /* MOVSHDUP {1,2,3,4} -> {2,2,4,4} (odd lanes duplicated). */
    v4sf sh = do_movshdup(a);
    acc += (int)sh[0];                     /* 2  -> acc 12 */

    /* MOVSLDUP {1,2,3,4} -> {1,1,3,3} (even lanes duplicated). */
    v4sf sl = do_movsldup(a);
    acc += (int)sl[2];                     /* 3  -> acc 15 */

    /* PMULLD {2,3,4,5} * {3,3,3,3} -> {6,9,12,15}. */
    v4si pa = { vol_two, 3, 4, 5 };
    v4si pb = { 3, 3, 3, 3 };
    v4si pm = do_pmulld(pa, pb);
    acc += pm[0];                          /* 6  -> acc 21 */

    /* PMINSD {10,-5,3,7} min {4,4,4,4} -> {4,-5,3,4}. */
    v4si na = { 10, -5, 3, 7 };
    v4si nb = { 4, 4, 4, 4 };
    v4si mn = do_pminsd(na, nb);
    acc += mn[2];                          /* 3  -> acc 24 */

    sys_exit(acc & 0xFF);
}
