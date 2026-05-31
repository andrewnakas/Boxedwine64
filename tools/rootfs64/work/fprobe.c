#include <stdio.h>
__attribute__((noinline)) static double getd(void){ volatile double d = 3.14159; return d; }
int main(void){
    double d = getd();
    printf("d=%.5f\n", d);
    printf("d2=%.2f\n", d*2.0);
    long i = (long)(d*100.0);   /* CVTTSD2SI: expect 314 */
    printf("i=%ld\n", i);
    return (int)(i & 0xFF);     /* 314 & 0xFF = 58 */
}
