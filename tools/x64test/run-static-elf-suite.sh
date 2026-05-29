#!/usr/bin/env bash
# Boxedwine64 — static-PIE ELF smoke suite.
#
# Cross-compiles a battery of small static-PIE x86_64 binaries with `zig cc`,
# then runs each through the in-tree --x64-run-elf harness. Confirms that
# the 64-bit interpreter, syscall64 dispatch, and ELF64 loader can carry
# real musl-built programs to a clean exit.
#
# Usage:  tools/x64test/run-static-elf-suite.sh [path-to-Boxedwine-binary]
# Default binary path: the Xcode Debug build at
#   ~/Library/Developer/Xcode/DerivedData/Boxedwine-*/Build/Products/Debug/Boxedwine.app/Contents/MacOS/Boxedwine
#
# Requires: zig (zig cc), Boxedwine built with BOXEDWINE_GUEST_X64=1.

set -e

BWX="${1:-}"
if [ -z "$BWX" ]; then
    BWX=$(ls -t ~/Library/Developer/Xcode/DerivedData/Boxedwine-*/Build/Products/Debug/Boxedwine.app/Contents/MacOS/Boxedwine 2>/dev/null | head -1)
fi
if [ ! -x "$BWX" ]; then
    echo "boxedwine binary not found (pass path or build via xcodebuild first)" >&2
    exit 2
fi

if ! command -v zig >/dev/null 2>&1; then
    echo "zig not installed (brew install zig)" >&2
    exit 2
fi

WORK="$(mktemp -d -t bw64test.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0
total=0

compile_and_run() {
    local name="$1"; local src="$2"; local expected="$3"
    total=$((total+1))
    printf "%s\n" "$src" > "$WORK/$name.c"
    if ! zig cc -target x86_64-linux-musl -static -O2 "$WORK/$name.c" -lm -o "$WORK/$name" 2>"$WORK/$name.err"; then
        echo "FAIL $name — compile error"
        cat "$WORK/$name.err"
        fail=$((fail+1))
        return
    fi
    out=$("$BWX" --x64-run-elf "$WORK/$name" 2>&1 || true)
    if echo "$out" | grep -q "exit reached cleanly" && echo "$out" | grep -qF "$expected"; then
        echo "PASS $name"
        pass=$((pass+1))
    else
        echo "FAIL $name — expected '$expected'"
        echo "$out" | tail -10
        fail=$((fail+1))
    fi
}

# --- test programs --------------------------------------------------------

compile_and_run hello '
#include <unistd.h>
int main(void){ write(1,"hello\n",6); return 0; }
' "hello"

compile_and_run sum '
#include <unistd.h>
int main(void){ long s=0; for(int i=1;i<=100;i++) s+=i; return (int)s; }
' "exit syscall, status=5050"

compile_and_run sieve '
#include <unistd.h>
int main(void){
    static char s[1001];
    for(int i=2;i<1001;i++) if(!s[i]) for(int j=2*i;j<1001;j+=i) s[j]=1;
    int c=0; for(int i=2;i<1001;i++) if(!s[i]) c++;
    return c; // 168
}
' "exit syscall, status=168"

compile_and_run fib25 '
#include <unistd.h>
long fib(long n){return n<2?n:fib(n-1)+fib(n-2);}
int main(void){ return (int)fib(25); } // 75025
' "exit syscall, status=75025"

compile_and_run qsort '
#include <unistd.h>
static int a[64]; static unsigned se=12345;
static int r7(void){ se=se*1103515245u+12345u; return (int)((se>>1)&0x7FFF); }
static void qs(int lo,int hi){
    if(lo>=hi)return;
    int p=a[(lo+hi)/2],i=lo,j=hi;
    while(i<=j){
        while(a[i]<p)i++;
        while(a[j]>p)j--;
        if(i<=j){int t=a[i];a[i]=a[j];a[j]=t;i++;j--;}
    }
    qs(lo,j); qs(i,hi);
}
int main(void){
    for(int i=0;i<64;i++) a[i]=r7();
    qs(0,63);
    for(int i=1;i<64;i++) if(a[i]<a[i-1]){write(1,"FAIL\n",5); return 1;}
    write(1,"OK\n",3); return 0;
}
' "OK"

compile_and_run strops '
#include <string.h>
#include <unistd.h>
int main(void){
    char src[128]; for(int i=0;i<127;i++) src[i]=(char)("A"[0]+(i%26)); src[127]=0;
    char dst[128]; memcpy(dst,src,128);
    if(memcmp(src,dst,128)!=0){write(1,"BAD\n",4);return 1;}
    if(strlen(src)!=127){write(1,"BAD\n",4);return 2;}
    write(1,"OK\n",3); return 0;
}
' "OK"

compile_and_run printf_int '
#include <stdio.h>
int main(void){
    int n=0;
    n+=printf("decimal: %d\n",12345);
    n+=printf("hex: %x %X\n",0xCAFE,0xBEEF);
    n+=printf("oct: %o\n",0755);
    n+=printf("str+int: %s=%d\n","answer",42);
    n+=printf("char+ld: %c %ld\n",(int)0x51,9999999999L);
    fflush(stdout);
    return n & 0xFF;
}
' "char+ld: Q 9999999999"

compile_and_run libm '
#include <math.h>
#include <unistd.h>
int main(void){
    double s = 0;
    for (int i = 1; i <= 100; i++) s += sqrt((double)i) + sin((double)i*0.01) + cos((double)i*0.01);
    long h = (long)(s * 1000.0);
    return (int)(h & 0xFFFF);
}
' "exit syscall, status=15337"

compile_and_run mmap '
#include <sys/mman.h>
#include <unistd.h>
int main(void){
    size_t n = 256*1024;
    void *p = mmap(0,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if (p==MAP_FAILED){write(1,"FAIL\n",5); return 1;}
    unsigned char *b=p;
    for (size_t i=0;i<n;i++) b[i]=(unsigned char)(i*31u);
    if (munmap(p,n)!=0){write(1,"FAIL\n",5); return 2;}
    write(1,"OK\n",3); return 0;
}
' "OK"

compile_and_run sigjmp '
#include <setjmp.h>
#include <unistd.h>
static jmp_buf env; static int sum;
static void inner(int n){ if(n==5)longjmp(env,42); sum+=n; inner(n+1); }
int main(void){
    if(setjmp(env)==0){inner(0); write(1,"NO\n",3); return 1;}
    if(sum!=10){write(1,"BAD\n",4); return 2;}
    write(1,"OK\n",3); return 0;
}
' "OK"

compile_and_run crc32 '
#include <unistd.h>
int main(void){
    static unsigned tab[256];
    for (unsigned i=0;i<256;i++){unsigned c=i; for(int k=0;k<8;k++)c=(c>>1)^(0xEDB88320u & -(c&1u)); tab[i]=c;}
    static unsigned char b[4096]; unsigned s=1;
    for(int i=0;i<4096;i++){s=s*1664525u+1013904223u; b[i]=(unsigned char)(s>>16);}
    unsigned crc=0xFFFFFFFFu;
    for(int i=0;i<4096;i++) crc=(crc>>8)^tab[(crc^b[i])&0xFF];
    crc=~crc;
    write(1,"OK\n",3);
    return (int)(crc & 0xFFFF);
}
' "exit syscall, status=36217"

compile_and_run hash '
#include <unistd.h>
typedef struct { unsigned long k; int v; char u; } S;
static S t[256];
static unsigned long mix(unsigned long k){
    k^=k>>33; k*=0xff51afd7ed558ccdULL;
    k^=k>>33; k*=0xc4ceb9fe1a85ec53ULL;
    k^=k>>33; return k;
}
static void put(unsigned long k,int v){
    unsigned long i=mix(k)&0xFF;
    while(t[i].u && t[i].k!=k) i=(i+1)&0xFF;
    t[i].k=k; t[i].v=v; t[i].u=1;
}
static int get(unsigned long k){
    unsigned long i=mix(k)&0xFF;
    while(t[i].u){ if(t[i].k==k) return t[i].v; i=(i+1)&0xFF; }
    return -1;
}
int main(void){
    for(int i=0;i<100;i++) put((unsigned long)i*0xdeadbeefULL+7,i*3);
    int s=0; for(int i=0;i<100;i++){int v=get((unsigned long)i*0xdeadbeefULL+7);if(v<0){write(1,"MISS\n",5);return 1;}s+=v;}
    if(s!=14850){write(1,"BAD\n",4); return 2;}
    write(1,"OK\n",3); return 0;
}
' "OK"

# --- summary --------------------------------------------------------------

echo ""
echo "=== static-ELF suite: $pass/$total PASS, $fail FAIL ==="
[ $fail -eq 0 ]
