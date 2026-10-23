/* sse4_diff: differential test for SSE3/SSSE3/SSE4.1/SSE4.2 (M20).
 * The SAME source builds two ways:
 *   - x86_64-linux ELF (freestanding) -> run under CPU64 (--x64-run-elf)
 *   - x86_64 macOS binary             -> run under Rosetta = reference
 * Each test feeds 256 PRNG-driven operand sets through one intrinsic and
 * prints "<name> <hash>"; any line that differs is a CPU64 bug.
 * Build: tools/x64test/sse4_diff.sh */
#include <nmmintrin.h>   /* SSE4.2 (pulls in SSE4.1/SSSE3/SSE3/SSE2) */
typedef unsigned long long u64; typedef unsigned int u32;

#ifdef __linux__
static long sys_write(int fd, const void* buf, long len) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "0"(1), "D"((long)fd), "S"(buf), "d"(len) : "rcx", "r11", "memory");
    return ret;
}
static void sys_exit(int status) __attribute__((noreturn));
static void sys_exit(int status) {
    __asm__ volatile ("syscall" : : "a"(60), "D"((long)status) : "rcx", "r11", "memory");
    __builtin_unreachable();
}
#else
#include <unistd.h>
#include <stdlib.h>
static long sys_write(int fd, const void* buf, long len) { return write(fd, buf, len); }
static void sys_exit(int status) { exit(status); }
#endif

static u64 xs = 0x9E3779B97F4A7C15ULL;
static u64 rnd(void) { xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17; return xs; }
static u64 H;
static void mixb(const void* p, int n) { const unsigned char* c = p; for (int i = 0; i < n; i++) { H ^= c[i]; H *= 1099511628211ULL; } }
static void mix64(u64 v) { mixb(&v, 8); }

static void put(const char* s) { int n = 0; while (s[n]) n++; sys_write(1, s, n); }
static void report(const char* name) {
    /* one write per line: the CPU64 runner echoes each write separately */
    char out[64]; int n = 0;
    while (name[n] && n < 40) { out[n] = name[n]; n++; }
    out[n++] = ' ';
    for (int i = 0; i < 16; i++) { u32 d = (u32)(H >> ((15 - i) * 4)) & 15; out[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); }
    out[n++] = '\n';
    sys_write(1, out, n);
}

/* integer vectors: random bytes, with a 1-in-4 chance of "interesting"
 * lanes (0, -1, 0x80, equal-to-other-operand) so compares/saturation/
 * sign cases all get hit */
static __m128i ri(void) {
    u64 lo = rnd(), hi = rnd();
    u64 k = rnd();
    if ((k & 3) == 0) lo &= 0x00FF00FF80FF0080ULL;
    if ((k & 12) == 0) hi = lo;
    return _mm_set_epi64x((long long)hi, (long long)lo);
}
/* float vectors from small integers / halves so results are exact-ish and NaN-free */
static __m128 rf(void) {
    return _mm_set_ps((float)((int)(rnd() % 2001) - 1000) / 4.0f, (float)((int)(rnd() % 2001) - 1000) / 8.0f,
                      (float)((int)(rnd() % 201) - 100), (float)((int)(rnd() % 2001) - 1000) / 2.0f);
}
static __m128d rd(void) {
    return _mm_set_pd((double)((long)(rnd() % 20001) - 10000) / 16.0, (double)((long)(rnd() % 2001) - 1000) / 2.0);
}

static unsigned char mem[64] __attribute__((aligned(16)));

#define N 256
#define TI(name, expr) do { H = 1469598103934665603ULL; for (int it = 0; it < N; it++) { \
    __m128i a = ri(), b = ri(), c = ri(); (void)a; (void)b; (void)c; \
    __m128i r_ = (expr); mixb(&r_, 16); } report(name); } while (0)
#define TF(name, expr) do { H = 1469598103934665603ULL; for (int it = 0; it < N; it++) { \
    __m128 a = rf(), b = rf(), c = rf(); (void)a; (void)b; (void)c; \
    __m128 r_ = (expr); mixb(&r_, 16); } report(name); } while (0)
#define TD(name, expr) do { H = 1469598103934665603ULL; for (int it = 0; it < N; it++) { \
    __m128d a = rd(), b = rd(), c = rd(); (void)a; (void)b; (void)c; \
    __m128d r_ = (expr); mixb(&r_, 16); } report(name); } while (0)
#define TS(name, expr) do { H = 1469598103934665603ULL; for (int it = 0; it < N; it++) { \
    __m128i a = ri(), b = ri(); (void)a; (void)b; \
    mix64((u64)(long long)(expr)); } report(name); } while (0)

void _start(void) __asm__("_start");
#ifndef __linux__
int main(void) { _start(); return 0; }
#endif
void _start(void) {
    /* SSE3 */
    TF("addsubps", _mm_addsub_ps(a, b));
    TD("addsubpd", _mm_addsub_pd(a, b));
    TF("haddps", _mm_hadd_ps(a, b));
    TD("haddpd", _mm_hadd_pd(a, b));
    TF("hsubps", _mm_hsub_ps(a, b));
    TD("hsubpd", _mm_hsub_pd(a, b));
    TD("movddup", _mm_movedup_pd(a));
    TF("movshdup", _mm_movehdup_ps(a));
    TF("movsldup", _mm_moveldup_ps(a));
    TI("lddqu", (_mm_storeu_si128((__m128i*)(mem + 3), a), _mm_lddqu_si128((const __m128i*)(mem + 3))));
    /* SSSE3 */
    TI("pshufb", _mm_shuffle_epi8(a, b));
    TI("phaddw", _mm_hadd_epi16(a, b));
    TI("phaddd", _mm_hadd_epi32(a, b));
    TI("phaddsw", _mm_hadds_epi16(a, b));
    TI("phsubw", _mm_hsub_epi16(a, b));
    TI("phsubd", _mm_hsub_epi32(a, b));
    TI("phsubsw", _mm_hsubs_epi16(a, b));
    TI("pmaddubsw", _mm_maddubs_epi16(a, b));
    TI("pmulhrsw", _mm_mulhrs_epi16(a, b));
    TI("psignb", _mm_sign_epi8(a, b));
    TI("psignw", _mm_sign_epi16(a, b));
    TI("psignd", _mm_sign_epi32(a, b));
    TI("pabsb", _mm_abs_epi8(a));
    TI("pabsw", _mm_abs_epi16(a));
    TI("pabsd", _mm_abs_epi32(a));
    TI("palignr3", _mm_alignr_epi8(a, b, 3));
    TI("palignr13", _mm_alignr_epi8(a, b, 13));
    TI("palignr20", _mm_alignr_epi8(a, b, 20));
    /* SSE4.1 */
    TI("pblendvb", _mm_blendv_epi8(a, b, c));
    TF("blendps", _mm_blend_ps(a, b, 0x5));
    TD("blendpd", _mm_blend_pd(a, b, 0x2));
    TF("blendvps", _mm_blendv_ps(a, b, c));
    TD("blendvpd", _mm_blendv_pd(a, b, c));
    TI("pblendw", _mm_blend_epi16(a, b, 0xA5));
    TI("pminsb", _mm_min_epi8(a, b));
    TI("pminsd", _mm_min_epi32(a, b));
    TI("pminuw", _mm_min_epu16(a, b));
    TI("pminud", _mm_min_epu32(a, b));
    TI("pmaxsb", _mm_max_epi8(a, b));
    TI("pmaxsd", _mm_max_epi32(a, b));
    TI("pmaxuw", _mm_max_epu16(a, b));
    TI("pmaxud", _mm_max_epu32(a, b));
    TI("pmulld", _mm_mullo_epi32(a, b));
    TI("pmuldq", _mm_mul_epi32(a, b));
    TI("pcmpeqq", _mm_cmpeq_epi64(a, b));
    TI("packusdw", _mm_packus_epi32(a, b));
    TI("pmovsxbw", _mm_cvtepi8_epi16(a));
    TI("pmovsxbd", _mm_cvtepi8_epi32(a));
    TI("pmovsxbq", _mm_cvtepi8_epi64(a));
    TI("pmovsxwd", _mm_cvtepi16_epi32(a));
    TI("pmovsxwq", _mm_cvtepi16_epi64(a));
    TI("pmovsxdq", _mm_cvtepi32_epi64(a));
    TI("pmovzxbw", _mm_cvtepu8_epi16(a));
    TI("pmovzxbd", _mm_cvtepu8_epi32(a));
    TI("pmovzxbq", _mm_cvtepu8_epi64(a));
    TI("pmovzxwd", _mm_cvtepu16_epi32(a));
    TI("pmovzxwq", _mm_cvtepu16_epi64(a));
    TI("pmovzxdq", _mm_cvtepu32_epi64(a));
    TS("ptestz", _mm_testz_si128(a, b));
    TS("ptestc", _mm_testc_si128(a, b));
    TS("ptestnzc", _mm_testnzc_si128(a, b));
    TS("pextrb", _mm_extract_epi8(a, 9));
    TS("pextrw", _mm_extract_epi16(a, 5));
    TS("pextrd", _mm_extract_epi32(a, 2));
    TS("pextrq", _mm_extract_epi64(a, 1));
    TS("extractps", _mm_extract_ps(_mm_castsi128_ps(a), 3));
    TI("pinsrb", _mm_insert_epi8(a, (int)_mm_cvtsi128_si32(b), 11));
    TI("pinsrd", _mm_insert_epi32(a, (int)_mm_cvtsi128_si32(b), 3));
    TI("pinsrq", _mm_insert_epi64(a, (long long)_mm_cvtsi128_si64(b), 1));
    TF("insertps", _mm_insert_ps(a, b, 0x9C));
    TF("roundps_n", _mm_round_ps(_mm_mul_ps(a, _mm_set1_ps(0.37f)), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
    TF("roundps_f", _mm_floor_ps(_mm_mul_ps(a, _mm_set1_ps(0.37f))));
    TF("roundps_c", _mm_ceil_ps(_mm_mul_ps(a, _mm_set1_ps(0.37f))));
    TF("roundps_t", _mm_round_ps(_mm_mul_ps(a, _mm_set1_ps(0.37f)), _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC));
    TD("roundpd_n", _mm_round_pd(_mm_mul_pd(a, _mm_set1_pd(0.37)), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
    TD("roundpd_f", _mm_floor_pd(_mm_mul_pd(a, _mm_set1_pd(0.37))));
    TF("roundss", _mm_round_ss(a, _mm_mul_ps(b, _mm_set1_ps(0.37f)), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
    TD("roundsd", _mm_round_sd(a, _mm_mul_pd(b, _mm_set1_pd(0.37)), _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC));
    TF("dpps", _mm_dp_ps(a, b, 0xF1));
    TF("dpps2", _mm_dp_ps(a, b, 0x7E));
    TD("dppd", _mm_dp_pd(a, b, 0x31));
    TI("mpsadbw0", _mm_mpsadbw_epu8(a, b, 0));
    TI("mpsadbw5", _mm_mpsadbw_epu8(a, b, 5));
    TI("phminposuw", _mm_minpos_epu16(a));
    TI("movntdqa", (_mm_store_si128((__m128i*)mem, a), _mm_stream_load_si128((__m128i*)mem)));
    /* SSE4.2 */
    TI("pcmpgtq", _mm_cmpgt_epi64(a, b));
    TS("crc32b", _mm_crc32_u8((u32)_mm_cvtsi128_si32(a), (unsigned char)_mm_cvtsi128_si32(b)));
    TS("crc32w", _mm_crc32_u16((u32)_mm_cvtsi128_si32(a), (unsigned short)_mm_cvtsi128_si32(b)));
    TS("crc32d", _mm_crc32_u32((u32)_mm_cvtsi128_si32(a), (u32)_mm_cvtsi128_si32(b)));
    TS("crc32q", _mm_crc32_u64((u64)_mm_cvtsi128_si64(a), (u64)_mm_cvtsi128_si64(b)));
    TS("pcmpistri_eqany", _mm_cmpistri(a, b, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY));
    TS("pcmpistri_ranges", _mm_cmpistri(a, b, _SIDD_UBYTE_OPS | _SIDD_CMP_RANGES | _SIDD_MOST_SIGNIFICANT));
    TS("pcmpistri_eqeach", _mm_cmpistri(a, b, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_EACH | _SIDD_NEGATIVE_POLARITY));
    TS("pcmpistri_eqord", _mm_cmpistri(a, b, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ORDERED));
    TS("pcmpistri_w", _mm_cmpistri(a, b, _SIDD_UWORD_OPS | _SIDD_CMP_EQUAL_ANY));
    TS("pcmpistri_sb", _mm_cmpistri(a, b, _SIDD_SBYTE_OPS | _SIDD_CMP_RANGES));
    TI("pcmpistrm_bit", _mm_cmpistrm(a, b, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY));
    TI("pcmpistrm_mask", _mm_cmpistrm(a, b, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_EACH | _SIDD_UNIT_MASK));
    TS("pcmpestri", _mm_cmpestri(a, (int)(rnd() % 20) - 2, b, (int)(rnd() % 20) - 2, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY));
    TS("pcmpestri_w", _mm_cmpestri(a, (int)(rnd() % 10), b, (int)(rnd() % 10), _SIDD_SWORD_OPS | _SIDD_CMP_RANGES));
    TI("pcmpestrm", _mm_cmpestrm(a, (int)(rnd() % 18), b, (int)(rnd() % 18), _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ORDERED | _SIDD_UNIT_MASK));
    TS("pcmpistrz", _mm_cmpistrz(a, b, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY));
    TS("pcmpistrc", _mm_cmpistrc(a, b, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY));
    TS("pcmpistro", _mm_cmpistro(a, b, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY));
    TS("pcmpistrs", _mm_cmpistrs(a, b, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY));
    TS("popcnt", __builtin_popcountll((u64)_mm_cvtsi128_si64(a)));
    put("DONE\n");
    sys_exit(0);
}
