/* Wider real-clang probe: same no-libc shape as hello_real.c but
 * exercises a broader compiler-emitted opcode surface so the cpu64
 * tracer can flag any missing opcodes that the narrow hello probe
 * didn't hit. The goal is *coverage*, not arithmetic — the final
 * exit status just needs to be deterministic.
 *
 * Shapes we want clang to emit:
 *   - Non-inlined function call (prologue/epilogue with push rbp / mov rbp,rsp / pop)
 *   - Recursion (call/ret discipline beyond one frame)
 *   - struct copy (the SysV ABI passes/returns structs in regs/stack)
 *   - switch (jump table or cascaded compare)
 *   - Bit ops (AND/OR/XOR/SHL/SHR via mask arithmetic)
 *   - Conditional move (CMOVcc — clang generates these for ternaries)
 *   - 64-bit memory ops via local arrays
 *
 * We do NOT touch libc / glibc / dynamic linking — this is still a
 * static-syscall-only probe. The point is opcode discovery against
 * compiler output, not full libc startup (that lives behind Milestone
 * A3 + rootfs, blocked on a real libc.so.6).
 */

static long sys_write(int fd, const char* buf, long len) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
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

/* Force out-of-line: clang inlines small leaves with -O2, which
 * collapses call/ret out of the probe. attribute noinline keeps the
 * call sequence in the emitted machine code. */
__attribute__((noinline))
static int classify(int x) {
    switch (x & 7) {
        case 0:  return x + 1;
        case 1:  return x * 2;
        case 2:  return x - 3;
        case 3:  return x ^ 0x55;
        case 4:  return x | 0x0F;
        case 5:  return x & 0x33;
        case 6:  return (x << 1) | (x >> 3);
        default: return x ? -x : 0;   /* ternary -> CMOVcc on clang */
    }
}

__attribute__((noinline))
static int recurse(int n) {
    if (n <= 0) return 0;
    /* Branch + recurse + arithmetic — exercises call/ret nesting. */
    return classify(n) + recurse(n - 1);
}

/* Small struct returned by value — SysV ABI returns ≤16B aggregates
 * in RAX:RDX. Exercises the multi-register return path. */
struct Pair { long lo; long hi; };

__attribute__((noinline))
static struct Pair make_pair(long a, long b) {
    struct Pair p;
    p.lo = a ^ b;
    p.hi = a + b;
    return p;
}

__attribute__((noinline))
static long sum_array(const long* arr, long n) {
    long acc = 0;
    /* Pointer-chase via SIB addressing forces 03 04 C5 or similar. */
    for (long i = 0; i < n; i++) {
        acc += arr[i];
    }
    return acc;
}

void _start(void) {
    static const char msg[] = "hello from wide-shape probe\n";
    sys_write(1, msg, sizeof(msg) - 1);

    /* Exercise call discipline, struct ABI, indexed loads. */
    long stack_arr[8] = { 11, 22, 33, 44, 55, 66, 77, 88 };
    long s = sum_array(stack_arr, 8);                    /* 396 */
    struct Pair p = make_pair(s, 7);                     /* lo=395, hi=403 */
    int r = recurse(5);                                  /* depends on classify */

    /* Combine via a ternary clang will fold to CMOVcc. */
    long final = (r > 0 ? p.lo + r : p.hi - r);
    long status = final & 0xFF;                          /* keep in U8 range */

    sys_exit((int)status);
}
