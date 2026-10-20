/* cpu_bench: deterministic CPU64 throughput + correctness probe (M19).
 * Mixes the shapes real wine/glibc code is made of: tight ALU loops, calls
 * and returns, indirect calls, byte/word loads (movzx/movsx), sign-extended
 * indices (movsxd), memcpy-ish copies, a sort, and string hashing.
 * Prints a 64-bit FNV hash of every intermediate result, so any semantic
 * divergence between interpreter and block executor changes the output.
 * Build: tools/buildRealClangElf.sh cpu_bench   Run: --x64-run-elf */
typedef unsigned long u64; typedef unsigned int u32; typedef unsigned char u8;
typedef signed char s8; typedef short s16; typedef int s32;

static long sys_write(int fd, const char* buf, long len) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "0"(1), "D"((long)fd), "S"(buf), "d"(len) : "rcx", "r11", "memory");
    return ret;
}
static void sys_exit(int status) __attribute__((noreturn));
static void sys_exit(int status) {
    __asm__ volatile ("syscall" : : "a"(60), "D"((long)status) : "rcx", "r11", "memory");
    __builtin_unreachable();
}

static u64 H = 1469598103934665603UL;
static void mix(u64 v) { for (int i = 0; i < 8; i++) { H ^= (u8)(v >> (i * 8)); H *= 1099511628211UL; } }

static u64 xs = 88172645463325252UL;
static u64 __attribute__((noinline)) rnd(void) { xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17; return xs; }

static u8 buf[65536];
static s32 arr[8192];

static u64 __attribute__((noinline)) sumBytes(const u8* p, s32 n) {
    u64 s = 0;
    for (s32 i = 0; i < n; i++) s += p[i] * (u64)(i & 7);
    return s;
}
static u64 __attribute__((noinline)) sumSigned(const u8* p, s32 n) {
    long s = 0;
    for (s32 i = 0; i + 1 < n; i += 2) s += (s8)p[i] + *(const s16*)(p + i);
    return (u64)s;
}
static void __attribute__((noinline)) copyBytes(u8* d, const u8* s, s32 n) { for (s32 i = 0; i < n; i++) d[i] = s[i] ^ (u8)i; }
static void __attribute__((noinline)) isort(s32* a, s32 n) {
    for (s32 i = 1; i < n; i++) { s32 v = a[i], j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
}
static u32 __attribute__((noinline)) strHash(const char* s) { u32 h = 5381; while (*s) h = h * 33 + (u8)*s++; return h; }

typedef u64 (*opfn)(u64, u64);
static u64 opAdd(u64 a, u64 b) { return a + b; }
static u64 opXor(u64 a, u64 b) { return a ^ (b << 3); }
static u64 opMul(u64 a, u64 b) { return a * (b | 1); }
static u64 opSub(u64 a, u64 b) { return a - (b >> 2); }
static opfn ops[4] = { opAdd, opXor, opMul, opSub };

void _start(void) {
    for (int i = 0; i < (int)sizeof(buf); i++) buf[i] = (u8)rnd();
    for (int round = 0; round < 40; round++) {
        mix(sumBytes(buf, sizeof(buf)));
        mix(sumSigned(buf, sizeof(buf)));
        copyBytes(buf + 32768, buf, 32768);
        for (int i = 0; i < 2048; i++) arr[i] = (s32)(rnd() & 0xFFFFF) - 0x80000;
        isort(arr, 2048);
        for (int i = 0; i < 2048; i += 97) mix((u64)(long)arr[i]);
        u64 acc = (u64)round;
        for (int i = 0; i < 200000; i++) acc = ops[(acc >> 7) & 3](acc, (u64)i);
        mix(acc);
        char name[32];
        for (int i = 0; i < 31; i++) name[i] = (char)('a' + (buf[i + round] % 26));
        name[31] = 0;
        for (int i = 0; i < 2000; i++) { name[i % 31] = (char)('a' + (i % 26)); mix(strHash(name)); }
    }
    char out[19]; out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++) { u32 d = (u32)(H >> ((15 - i) * 4)) & 15; out[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); }
    out[18] = '\n';
    sys_write(1, out, 19);
    sys_exit(0);
}
