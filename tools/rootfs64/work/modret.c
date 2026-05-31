/* Isolate the probe2 exit-status bug: return (int)(N % 100) where N is a
 * large runtime value, compiled to the magic-number signed division. */
__attribute__((noinline)) static long getN(void){ volatile long n=38480; return n; }
int main(void){ long n=getN(); return (int)(n % 100); }  /* expect exit 80 */
