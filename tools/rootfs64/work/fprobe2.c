#include <stdio.h>
/* Isolate WHERE %f digit generation breaks. Each value is volatile so the
   compiler cannot constant-fold the printf. We test:
   - exact-in-double values (0.5, 0.25, 1.5, 100.0)
   - the failing 3.14159 at increasing precision
   so we can see if it's precision-count dependent or value dependent. */
__attribute__((noinline)) static double vd(double x){ volatile double d = x; return d; }
int main(void){
    printf("a=%.5f\n", vd(0.5));      /* 0.50000 */
    printf("b=%.5f\n", vd(1.5));      /* 1.50000 */
    printf("c=%.5f\n", vd(100.0));    /* 100.00000 */
    printf("e=%.1f\n", vd(3.14159));  /* 3.1 */
    printf("f=%.2f\n", vd(3.14159));  /* 3.14 */
    printf("g=%.3f\n", vd(3.14159));  /* 3.142 */
    printf("h=%.4f\n", vd(3.14159));  /* 3.1416 */
    printf("i=%.5f\n", vd(3.14159));  /* 3.14159 */
    printf("j=%.5f\n", vd(2.0));      /* 2.00000 */
    return 0;
}
