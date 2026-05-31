// join_probe — fast pthread_join wakeup test for Boxedwine64's KThread64.
//
// Unlike pthread_probe (which hammers a mutex 200k times to stress atomics),
// this does almost no work in the worker: it just sets a flag and returns.
// The whole point is to confirm that the JOIN wakes — i.e. that the worker's
// exit (clear_child_tid64 zero + FUTEX_WAKE) wakes the main thread blocked in
// pthread_join's lll_wait_tid. If join hangs, this hangs; if join returns,
// we print the worker's return value and exit 0.
//
// Build (Debian bookworm amd64):
//   docker run --rm --platform linux/amd64 -v "$PWD":/w -w /w debian:bookworm \
//     sh -c 'apt-get update && apt-get install -y gcc && \
//            gcc -O2 -pthread join_probe.c -o join_probe'
#include <pthread.h>
#include <stdio.h>

static volatile int ran = 0;

static void* worker(void* arg) {
    (void)arg;
    ran = 1;
    return (void*)0x1234;
}

int main(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) != 0) {
        printf("pthread_create FAILED\n");
        return 2;
    }
    void* ret = (void*)0;
    printf("join_probe: created, joining...\n");
    int rc = pthread_join(t, &ret);
    printf("join_probe: joined rc=%d ran=%d ret=%p\n", rc, ran, ret);
    if (rc == 0 && ran == 1 && ret == (void*)0x1234) {
        printf("join_probe: PASS\n");
        return 0;
    }
    printf("join_probe: FAIL\n");
    return 1;
}
