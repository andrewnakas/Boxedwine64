/* Real-clang probe: install SIGUSR1 handler via rt_sigaction, raise
 * SIGUSR1 via tgkill, verify the handler ran by checking a global
 * sentinel, then exit with the sentinel as status.
 *
 * Exercises the Milestone B signal-delivery path end-to-end:
 *   1. rt_sigaction(SIGUSR1, {handler, ...}) — registers handler
 *   2. tgkill(pid, tid, SIGUSR1)              — kernel delivers
 *   3. deliverSignalSync builds the x86-64 signal frame, sets RIP=handler
 *   4. handler runs, writes sentinel, ends with rt_sigreturn
 *   5. restoreSignalFrame restores RIP, execution continues
 *   6. main reads sentinel, exits with it as status
 *
 * If any of those steps is broken the sentinel stays 0 and we exit 99
 * (deliberately wrong); pass = exit 77 (handler observed sentinel write).
 *
 * Static syscalls only — no libc. */

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

static long sys_getpid(void) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "0"(39) : "rcx", "r11", "memory");
    return ret;
}

static long sys_gettid(void) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "0"(186) : "rcx", "r11", "memory");
    return ret;
}

static long sys_tgkill(long tgid, long tid, int sig) {
    /* tgkill is a 3-arg syscall: tgid=RDI, tid=RSI, sig=RDX. */
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret)
        : "0"(234), "D"(tgid), "S"(tid), "d"((long)sig)
        : "rcx", "r11", "memory");
    return ret;
}

/* Kernel-style x86-64 sigaction struct.
 *   { handler; sa_flags; restorer; sa_mask }
 * sa_flags must include SA_RESTORER (0x04000000); restorer is the
 * trampoline that calls rt_sigreturn. We bundle a tiny restorer
 * here so we don't need libc's. */
struct k_sigaction {
    void (*sa_handler)(int);
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    unsigned long sa_mask;
};

#define SA_RESTORER  0x04000000
#define SIGUSR1      10

static long sys_rt_sigaction(int sig, const struct k_sigaction* act,
                              struct k_sigaction* old) {
    long ret;
    register long r10 __asm__("r10") = 8L;   /* sigsetsize */
    __asm__ volatile ("syscall" : "=a"(ret)
        : "0"(13), "D"((long)sig), "S"(act), "d"(old), "r"(r10)
        : "rcx", "r11", "memory");
    return ret;
}

/* Tiny restorer: `mov rax, 15; syscall` = rt_sigreturn. */
extern void sigreturn_trampoline(void);
__asm__(
    ".global sigreturn_trampoline\n"
    "sigreturn_trampoline:\n"
    "    mov $15, %rax\n"
    "    syscall\n"
);

/* Volatile so clang doesn't fold the read in main below. */
static volatile long g_sentinel = 0;

static void handler(int sig) {
    /* If we got here, the signal-delivery + frame-build path worked. */
    (void)sig;
    static const char m[] = "[handler ran]\n";
    sys_write(1, m, sizeof(m) - 1);
    g_sentinel = 77;
}

void _start(void) {
    static const char msg[] = "signal probe: installing SIGUSR1 handler\n";
    sys_write(1, msg, sizeof(msg) - 1);

    struct k_sigaction act = { 0 };
    act.sa_handler = handler;
    act.sa_flags   = SA_RESTORER;
    act.sa_restorer = sigreturn_trampoline;
    long r = sys_rt_sigaction(SIGUSR1, &act, 0);
    if (r != 0) sys_exit(91);   /* rt_sigaction failed */

    long pid = sys_getpid();
    long tid = sys_gettid();
    static const char m2[] = "signal probe: raising SIGUSR1 via tgkill\n";
    sys_write(1, m2, sizeof(m2) - 1);

    r = sys_tgkill(pid, tid, SIGUSR1);
    if (r != 0) sys_exit(92);   /* tgkill failed */

    /* Sentinel should now be 77 — handler ran. */
    long status = g_sentinel ? g_sentinel : 99;
    sys_exit((int)status);
}
