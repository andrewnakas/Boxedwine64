/* Middle DSO. The exe (hello_chain) imports chain_compute from us; we
 * in turn import chain_leaf from libchainB. The two-hop chain forces
 * the loader to:
 *   1. Resolve chain_compute (exe -> A) — direct DT_NEEDED.
 *   2. Resolve chain_leaf (A -> B)        — DT_NEEDED *of a DSO*.
 *   3. Flat symbol table must lookup chain_leaf across all loaded
 *      objects when we process A's relocations, even though chain_leaf
 *      is never mentioned in the exe.
 *
 * No libc. */

extern long chain_leaf(long x);

long chain_compute(long x) {
    /* Call into libchainB. If the symbol wasn't resolved, the guest CALL
     * through PLT->GOT lands in 0x0 and segfaults. */
    long leaf = chain_leaf(x);
    return leaf + (x * 2);
}
