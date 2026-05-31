#include <stdio.h>
/* Hypothesis: the FIRST high-precision %f call collapses to 0; later calls
   are fine (lazy-init bug). Print the SAME value three times. */
__attribute__((noinline)) static double vd(double x){ volatile double d = x; return d; }
int main(void){
    printf("first =%.5f\n", vd(0.5));   /* if first-call bug: 0.00000 */
    printf("second=%.5f\n", vd(0.5));   /* expect 0.50000 */
    printf("third =%.5f\n", vd(0.5));   /* expect 0.50000 */
    return 0;
}
