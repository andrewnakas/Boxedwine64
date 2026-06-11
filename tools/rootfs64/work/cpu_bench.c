// CPU-bound microbenchmark: a tight loop with prefixed instructions (REX +
// 64-bit ops) — exactly what the prefix-decode cache targets. No syscalls in
// the hot loop, no I/O, so wall time reflects interpreter throughput.
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
int main(void) {
    volatile uint64_t acc = 0;
    uint64_t x = 0x123456789abcdef0ULL;
    for (uint64_t i = 0; i < 40000000ULL; i++) {
        x ^= (x << 13);
        x ^= (x >> 7);
        x ^= (x << 17);
        acc += x + i;
        acc ^= (acc * 2654435761ULL);
    }
    char buf[32];
    int n = 0; uint64_t v = acc;
    for (int k = 0; k < 16; k++) { buf[15-k] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
    buf[16] = '\n';
    write(1, buf, 17);
    return 0;
}
