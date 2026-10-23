/* hello64: static x86_64 ELF, M21 mixed-arch probe.
 * argv[1] (optional) = path of a program to execve next (e.g. hello32). */
static long sys3(long nr, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "0"(nr), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static long slen(const char* s) { long n = 0; while (s[n]) n++; return n; }
void cstart(long* sp) {
    long argc = sp[0];
    char** argv = (char**)(sp + 1);
    char** envp = argv + argc + 1;
    const char* msg = "hello from x86_64 (syscall)\n";
    sys3(1, 1, (long)msg, slen(msg));
    if (argc > 1) {
        char* nargv[3] = {argv[1], argc > 2 ? argv[2] : 0, 0};
        long r = sys3(59, (long)argv[1], (long)nargv, (long)envp);
        const char* bad = "hello64: execve failed\n";
        sys3(1, 1, (long)bad, slen(bad));
        sys3(60, 100 + (-r), 0, 0);
    }
    sys3(60, 0, 0, 0);
}
__asm__(".globl _start\n_start:\n  mov %rsp, %rdi\n  and $-16, %rsp\n  call cstart\n");
