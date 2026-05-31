// clone_flag — simplest possible: child sets flag=1 and exits; parent spins
// reading the shared flag (no futex), then prints. Confirms (a) the child
// actually executes, and (b) child writes are visible to the parent via the
// shared CLONE_VM memory. No futex involved.
#include <stdint.h>
#define SYS_write 1
#define SYS_exit 60
#define SYS_exit_group 231
#define SYS_clone 56
#define CLONE_VM 0x100
#define CLONE_FS 0x200
#define CLONE_FILES 0x400
#define CLONE_SIGHAND 0x800
#define CLONE_THREAD 0x10000

static volatile int flag = 0;
static char cstk[65536] __attribute__((aligned(16)));

static inline long sc(long n,long a,long b,long c,long d,long e,long f){
    long r;register long r10 __asm__("r10")=d;register long r8 __asm__("r8")=e;register long r9 __asm__("r9")=f;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");return r;}

void _start(void){
    long flags=CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD;
    long sp=(long)(cstk+sizeof(cstk));
    register long r10 __asm__("r10")=0, r8 __asm__("r8")=0, r9 __asm__("r9")=0;
    long tid;
    __asm__ volatile("syscall":"=a"(tid)
        :"a"(SYS_clone),"D"(flags),"S"(sp),"d"(0),"r"(r10),"r"(r8),"r"(r9)
        :"rcx","r11","memory");
    if(tid==0){
        flag=1;
        sc(SYS_exit,0,0,0,0,0,0);
        for(;;){}
    }
    // Parent spins (bounded) waiting for the child to set flag.
    for(volatile long i=0;i<50000000 && flag==0;i++){}
    sc(SYS_write,1,(long)(flag?"flag-set\n":"flag-zero\n"), flag?9:10, 0,0,0);
    sc(SYS_exit_group,0,0,0,0,0,0);
}
