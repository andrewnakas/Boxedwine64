/* probe2 — a non-trivial dynamically-linked glibc program for exercising the
 * full-kernel dynamic path (real ld-linux + libc.so.6), beyond hello-world.
 * Exercises: malloc/free churn, memset/strlen, getenv, snprintf with %.5f
 * (the real __printf_fp float path), and a sort. Build & place in the rootfs:
 *
 *   docker run --rm --platform linux/amd64 -v "$PWD:/w" gcc:13 \
 *     bash -c 'cd /w && gcc -O2 -o probe2 probe2.c'
 *   cp probe2 ../root/bin/probe2
 *
 * Run: tools/run_x64_root.sh /bin/probe2   (expects exit 80)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    /* malloc/free churn with content. */
    char* bufs[64];
    for (int i = 0; i < 64; i++) {
        bufs[i] = malloc((i + 1) * 37);
        memset(bufs[i], 'A' + (i % 26), (i + 1) * 37 - 1);
        bufs[i][(i + 1) * 37 - 1] = 0;
    }
    long sum = 0;
    for (int i = 0; i < 64; i++) { sum += strlen(bufs[i]); free(bufs[i]); }

    const char* path = getenv("PATH");

    char out[128];
    double pi = 3.14159265358979;
    snprintf(out, sizeof out, "pi=%.5f sum=%ld path=%s",
             pi, sum, path ? path : "(null)");
    printf("%s\n", out);

    int a[] = {5, 3, 9, 1, 7, 2, 8, 4, 6, 0};
    for (int i = 0; i < 10; i++)
        for (int j = i + 1; j < 10; j++)
            if (a[j] < a[i]) { int t = a[i]; a[i] = a[j]; a[j] = t; }
    printf("sorted[0]=%d sorted[9]=%d\n", a[0], a[9]);

    return (int)(sum % 100);   /* 38480 % 100 = 80 */
}
