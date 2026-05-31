// clone_ok — CORRECT raw-clone threading test. The clone syscall is emitted
// INLINE in _start (no wrapper function), so the child never executes a `ret`
// against its fresh stack (the classic raw-clone pitfall). Child increments a
// shared counter and signals via futex; parent waits, prints, exits.
//
// Build: docker run --rm --platform linux/amd64 -v "$PWD":/w -w /w \
//   debian:bookworm sh -c 'apt-get update && apt-get install -y gcc && \
//   gcc -O2 -static -nostartfiles -ffreestanding clone_ok.c -o clone_ok'
#include <stdint.h>

#define SYS_write 1
#define SYS_exit 60
#define SYS_exit_group 231
#define SYS_futex 202
#define SYS_clone 56
#define CLONE_VM 0x100
#define CLONE_FS 0x200
#define CLONE_FILES 0x400
#define CLONE_SIGHAND 0x800
#define CLONE_THREAD 0x10000
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

static volatile int futex_done = 0;
static volatile long counter = 0;
static char cstk[65536] __attribute__((aligned(16)));

// Force-inline syscall so the child path contains no call/ret.
static inline long sc(long n,long a,long b,long c,long d,long e,long f){
    long r;register long r10 __asm__("r10")=d;register long r8 __asm__("r8")=e;register long r9 __asm__("r9")=f;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");return r;}

static void out(const char* s,long n){ sc(SYS_write,1,(long)s,n,0,0,0); }
static void out_num(long v){ char b[24]; int i=24; b[--i]='\n'; if(!v)b[--i]='0';
    while(v>0&&i>0){b[--i]='0'+(v%10);v/=10;} out(b+i,24-i); }

void _start(void){
    long flags=CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD;
    // Inline clone syscall. After it, RAX=0 in child, =tid in parent. We branch
    // immediately with NO intervening call/ret in either path.
    register long r10 __asm__("r10")=0;       // parent_tid
    register long r8  __asm__("r8") =0;        // tls
    register long r9  __asm__("r9") =0;        // child_tid
    long sp=(long)(cstk+sizeof(cstk));
    long tid;
    __asm__ volatile("syscall"
        :"=a"(tid)
        :"a"(SYS_clone),"D"(flags),"S"(sp),"d"(0),"r"(r10),"r"(r8),"r"(r9)
        :"rcx","r11","memory");

    if(tid==0){
        // CHILD: runs on cstk. No ret — loops then exits via syscall.
        for(int i=0;i<100000;i++) counter++;
        futex_done=1;
        sc(SYS_futex,(long)&futex_done,FUTEX_WAKE,1,0,0,0);
        sc(SYS_exit,0,0,0,0,0,0);
        for(;;){}  // unreachable; never ret
    }

    // PARENT.
    while(futex_done==0)
        sc(SYS_futex,(long)&futex_done,FUTEX_WAIT,0,0,0,0);
    out("counter=",8);
    out_num(counter);
    long ok=(counter==100000);
    out(ok?"PASS\n":"FAIL\n",5);
    sc(SYS_exit_group, ok?0:1, 0,0,0,0,0);
}
