/* Multi-signal probe — extends hello_signal (single SIGUSR1) to prove the
 * signal infrastructure generalizes:
 *   1. Distinct handlers for two different signals (SIGUSR1=10, SIGUSR2=12).
 *   2. The handler receives the correct signal number in its `sig` arg
 *      (RDI per the SysV signal-handler ABI) — not just "a handler ran".
 *   3. Two sequential deliver→handle→sigreturn cycles on the same thread
 *      leave the main flow intact (RSP/regs restored correctly each time).
 *
 * Each handler records (signal_number * 1) into a per-signal slot. Main
 * sums the slots and exits with the total:
 *   SIGUSR1 handler sees sig=10 -> slot1=10
 *   SIGUSR2 handler sees sig=12 -> slot2=12
 *   exit = 10 + 12 = 22
 * Any wrong value means a sig-arg or sigreturn-restore bug.
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
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret)
        : "0"(234), "D"(tgid), "S"(tid), "d"((long)sig)
        : "rcx", "r11", "memory");
    return ret;
}

struct k_sigaction {
    void (*sa_handler)(int);
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    unsigned long sa_mask;
};

#define SA_RESTORER  0x04000000
#define SIGUSR1      10
#define SIGUSR2      12

static long sys_rt_sigaction(int sig, const struct k_sigaction* act,
                              struct k_sigaction* old) {
    long ret;
    register long r10 __asm__("r10") = 8L;
    __asm__ volatile ("syscall" : "=a"(ret)
        : "0"(13), "D"((long)sig), "S"(act), "d"(old), "r"(r10)
        : "rcx", "r11", "memory");
    return ret;
}

extern void sigreturn_trampoline(void);
__asm__(
    ".global sigreturn_trampoline\n"
    "sigreturn_trampoline:\n"
    "    mov $15, %rax\n"
    "    syscall\n"
);

static volatile long g_slot1 = 0;
static volatile long g_slot2 = 0;

/* Both handlers record their received sig arg, proving RDI carried the
 * right value through deliverSignalSync. */
static void handler1(int sig) { g_slot1 = sig; }   /* expect sig=10 */
static void handler2(int sig) { g_slot2 = sig; }   /* expect sig=12 */

static void install(int sig, void (*h)(int)) {
    struct k_sigaction act = { 0 };
    act.sa_handler = h;
    act.sa_flags = SA_RESTORER;
    act.sa_restorer = sigreturn_trampoline;
    if (sys_rt_sigaction(sig, &act, 0) != 0) sys_exit(91);
}

void _start(void) {
    static const char msg[] = "sig2 probe: two distinct signal handlers\n";
    sys_write(1, msg, sizeof(msg) - 1);

    install(SIGUSR1, handler1);
    install(SIGUSR2, handler2);

    long pid = sys_getpid();
    long tid = sys_gettid();

    if (sys_tgkill(pid, tid, SIGUSR1) != 0) sys_exit(92);
    if (sys_tgkill(pid, tid, SIGUSR2) != 0) sys_exit(93);

    long total = g_slot1 + g_slot2;   /* 10 + 12 = 22 */
    sys_exit((int)(total & 0xFF));
}
