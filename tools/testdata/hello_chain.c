/* Main exe in a two-hop dynamic-link chain. Imports chain_compute()
 * from libchainA. libchainA in turn imports chain_leaf() from libchainB.
 *
 *   exe -> libchainA -> libchainB
 *
 * For x=5:
 *   chain_leaf(5)    = 5*7 + 3       = 38
 *   chain_compute(5) = 38 + 5*2      = 48
 *
 * Exit with the low byte of the result (48). Anything else means a
 * relocation was wrong, the DT_NEEDED-of-a-DSO recursion didn't fire,
 * or the flat symbol table missed a leaf-DSO symbol.
 *
 * No libc — sys_write/sys_exit are inline asm. */

extern long chain_compute(long x);

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
    static const char msg[] = "chain probe: exe -> libchainA -> libchainB\n";
    sys_write(1, msg, sizeof(msg) - 1);

    long r = chain_compute(5);   /* 48 */
    long status = r & 0xFF;       /* 48 */
    sys_exit((int)status);
}
