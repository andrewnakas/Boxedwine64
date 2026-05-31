/* dirprobe — a real dynamically-linked (libc-only) directory lister, the
   coreutils-ls essence without libselinux/libpcre2. Exercises opendir/readdir
   (getdents64) + stat through real dynamic glibc. */
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>

static int cmp(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "/";
    DIR* d = opendir(path);
    if (!d) { printf("opendir(%s) failed\n", path); return 2; }
    char* names[256];
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && n < 256) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof(names[0]), cmp);
    for (int i = 0; i < n; i++) { printf("%s\n", names[i]); free(names[i]); }
    printf("count=%d\n", n);
    return 0;
}
