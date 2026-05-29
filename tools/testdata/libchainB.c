/* Leaf DSO in a two-hop dynamic-link chain. The middle DSO (libchainA)
 * imports `chain_leaf` from us via DT_NEEDED. Proves the flat symbol
 * table's cross-DSO resolution works when the *exe* doesn't directly
 * reference us — only the *library* does.
 *
 * If linkSharedObjectsRecursive only resolved symbols against the exe's
 * imports, this would crash: chain_leaf would be unresolved in libchainA.
 *
 * No libc. */

long chain_leaf(long x) {
    /* Distinctive arithmetic so the verifier can be unambiguous. */
    return (x * 7) + 3;
}
