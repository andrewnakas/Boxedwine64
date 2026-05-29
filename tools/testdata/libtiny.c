/* Tiny shared library providing one symbol the main exe imports
 * via DT_NEEDED + R_X86_64_JUMP_SLOT. Proves the end-to-end
 * dynamic-linking path: PT_DYNAMIC parse on both files, DT_NEEDED
 * recursion, JUMP_SLOT relocation against a resolved symbol, then
 * a guest CALL through PLT->GOT lands in libtiny's code.
 *
 * No libc, no startup files — _start is not defined here because
 * this is a .so, not an exe. */

long tiny_compute(long x) {
    /* Some arithmetic worth verifying. */
    long acc = 0;
    for (long i = 0; i < x; i++) acc += i * 3 - 1;
    return acc;
}
