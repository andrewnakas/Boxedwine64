/* Main dynamic exe that imports tiny_compute() from libtiny.so via
 * DT_NEEDED. The compiler emits a PLT stub for the call; the linker
 * sets up an R_X86_64_JUMP_SLOT against the GOT slot; ld.so (or our
 * eager binder) resolves the slot to libtiny's symbol; the CALL
 * indirect-through-GOT lands in tiny_compute.
 *
 * This is the closest reachable proof of Milestone A's exit
 * criterion: a dynamically-linked binary, with DT_NEEDED resolved,
 * with cross-DSO symbol calls succeeding, ending in a clean
 * sys_exit.
 *
 * Still no libc — sys_write/sys_exit are inline asm. The dynamic
 * linkage we exercise is *just* the tiny shared lib we built
 * alongside this exe. */

extern long tiny_compute(long x);

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

void _start(void) {
    static const char msg[] = "dynlink probe: calling libtiny\n";
    sys_write(1, msg, sizeof(msg) - 1);

    long r = tiny_compute(10);   /* sum i=0..9 of (3i-1) = 3*45 - 10 = 125 */
    long status = r & 0xFF;       /* 125 */
    sys_exit((int)status);
}
