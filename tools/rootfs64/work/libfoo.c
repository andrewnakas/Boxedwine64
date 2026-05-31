#include <stdio.h>
int foo_value(void) { return 42; }
void foo_hello(void) { fputs("libfoo: hello from versioned DSO\n", stdout); }
