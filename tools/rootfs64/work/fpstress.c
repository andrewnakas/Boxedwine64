#include <stdio.h>
/* Stress the PLT-resolver FP-arg path: the FIRST printf takes 4 doubles, so
   XMM0..3 must all survive _dl_runtime_resolve_fxsave; then negatives, large
   and small magnitudes at various precisions. No libm (rootfs has libc only). */
__attribute__((noinline)) static double vd(double x){ volatile double d = x; return d; }
int main(void){
    printf("multi: %.3f %.3f %.3f %.3f\n", vd(1.111), vd(2.222), vd(3.333), vd(4.444));
    printf("neg:   %.5f\n", vd(-2.71828));
    printf("big:   %.2f\n", vd(123456.789));
    printf("small: %.8f\n", vd(0.000123456));
    printf("e:     %.10f\n", vd(2.718281828459045));
    return 0;
}
