// pthread_probe — minimal threading smoke test for Boxedwine64's KThread64.
//
// Spawns one worker thread that increments a shared counter under a mutex,
// the main thread joins it, then prints the result and exits 0 on success.
// Exercises clone (pthread_create), futex (mutex lock/unlock + join wakeup),
// set_tid_address/CHILD_CLEARTID (join), and thread exit.
//
// Build (matches the other rootfs64 dynamic probes, Debian bookworm amd64):
//   docker run --rm -v "$PWD":/w -w /w debian:bookworm \
//     sh -c 'apt-get update && apt-get install -y gcc && \
//            gcc -O2 -pthread pthread_probe.c -o pthread_probe'
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter = 0;

static void* worker(void* arg) {
    long n = (long)arg;
    for (long i = 0; i < n; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return (void*)counter;
}

int main(void) {
    const long N = 100000;
    pthread_t t;
    if (pthread_create(&t, NULL, worker, (void*)N) != 0) {
        printf("pthread_create FAILED\n");
        return 2;
    }
    // Main thread also bumps the counter, racing the worker through the mutex.
    for (long i = 0; i < N; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    void* ret;
    pthread_join(t, &ret);

    printf("counter=%ld (expected %ld) join_ret=%ld\n", counter, 2 * N, (long)ret);
    if (counter == 2 * N) {
        printf("pthread_probe: PASS\n");
        return 0;
    }
    printf("pthread_probe: FAIL\n");
    return 1;
}
