/* Static-PIE-ish hello: no libc, syscalls inline, ELF64.
 * Built via macOS clang cross-targeting x86_64-linux-gnu.
 * Purpose: feed real compiler-emitted code into the cpu64 tracer
 * so we discover which opcodes glibc-shape code uses that our
 * hand-coded discovery ELFs missed. */
static long sys_write(int fd, const char* buf, long len) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"(1), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static void sys_exit(int status) __attribute__((noreturn));
static void sys_exit(int status) {
    __asm__ volatile (
        "syscall"
        :
        : "a"(60), "D"((long)status)
        : "rcx", "r11", "memory"
    );
    __builtin_unreachable();
}

/* Return value rolls through a small bit of computed arithmetic so the
 * compiler has to emit MOV/ADD/SHL/CMP/conditional-branch, not just two
 * syscalls back-to-back. */
static int compute(void) {
    int acc = 0;
    for (int i = 1; i <= 10; i++) {
        acc += i * 2;
        if (acc > 50) acc -= 3;
    }
    return acc; /* 1*2+...+10*2 = 110, minus three 3s after thresholds = 101 */
}

void _start(void) {
    const char msg[] = "hello from real clang\n";
    sys_write(1, msg, sizeof(msg) - 1);
    sys_exit(compute());
}
