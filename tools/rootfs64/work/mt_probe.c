// mt_probe — multi-thread correctness probe that exercises the per-address
// sharded atomic locks (unlike pthread_probe, which deliberately hammers ONE
// shared mutex word and so can never benefit from address sharding).
//
// Four worker threads each spin on their OWN counter (independent cache lines /
// pages worth of atomics where possible) a modest number of times, then a
// single shared atomic total is bumped once per thread under a shared mutex.
// Main joins all four. PASS requires every per-thread counter correct, the
// shared total correct, and all joins to return — i.e. clone + TLS + atomics on
// distinct addresses + join wakeup all working, without the 200k-on-one-word
// serialization that makes pthread_probe crawl.
//
// Build (Debian bookworm amd64):
//   docker run --rm --platform linux/amd64 -v "$PWD":/w -w /w debian:bookworm \
//     sh -c 'apt-get update && apt-get install -y gcc && \
//            gcc -O2 -pthread mt_probe.c -o mt_probe'
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>

#define NTHREAD 4
#define ITERS   2000

static atomic_long per[NTHREAD];
static pthread_mutex_t total_lock = PTHREAD_MUTEX_INITIALIZER;
static long total = 0;

static void* worker(void* arg) {
    long idx = (long)arg;
    for (int i = 0; i < ITERS; i++) {
        atomic_fetch_add(&per[idx], 1);   // distinct address per thread
    }
    pthread_mutex_lock(&total_lock);
    total += atomic_load(&per[idx]);
    pthread_mutex_unlock(&total_lock);
    return (void*)(long)idx;
}

int main(void) {
    pthread_t t[NTHREAD];
    for (long i = 0; i < NTHREAD; i++) {
        if (pthread_create(&t[i], NULL, worker, (void*)i) != 0) {
            printf("mt_probe: pthread_create %ld FAILED\n", i);
            return 2;
        }
    }
    int ok = 1;
    for (long i = 0; i < NTHREAD; i++) {
        void* ret = (void*)-1;
        pthread_join(t[i], &ret);
        if ((long)ret != i) { printf("mt_probe: join %ld ret=%ld bad\n", i, (long)ret); ok = 0; }
        if (atomic_load(&per[i]) != ITERS) { printf("mt_probe: per[%ld]=%ld bad\n", i, atomic_load(&per[i])); ok = 0; }
    }
    long expected = (long)NTHREAD * ITERS;
    printf("mt_probe: total=%ld (expected %ld)\n", total, expected);
    if (ok && total == expected) {
        printf("mt_probe: PASS\n");
        return 0;
    }
    printf("mt_probe: FAIL\n");
    return 1;
}
