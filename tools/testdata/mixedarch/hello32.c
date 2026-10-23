/* hello32: static i386 ELF (int $0x80), M21 mixed-arch probe.
 * argv[1] (optional) = path of a program to execve next (e.g. hello64). */
static int sys3(int nr, int a, int b, int c) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "0"(nr), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }
__attribute__((force_align_arg_pointer)) void cstart(int* sp) {
    int argc = sp[0];
    char** argv = (char**)(sp + 1);
    char** envp = argv + argc + 1;
    const char* msg = "hello from i386 (int 0x80)\n";
    sys3(4, 1, (int)msg, slen(msg));                 /* write */
    if (argc > 1) {
        char* nargv[2] = {argv[1], 0};
        int r = sys3(11, (int)argv[1], (int)nargv, (int)envp); /* execve */
        const char* bad = "hello32: execve failed\n";
        sys3(4, 1, (int)bad, slen(bad));
        sys3(1, 100 + (-r), 0, 0);
    }
    sys3(1, 0, 0, 0);                                  /* exit(0) */
}
__asm__(".globl _start\n_start:\n  mov %esp, %eax\n  push %eax\n  call cstart\n");
