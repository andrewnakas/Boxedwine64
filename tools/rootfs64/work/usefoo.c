#include <stdio.h>
int foo_value(void);
void foo_hello(void);
int main(void) {
    foo_hello();
    int v = foo_value();
    printf("usefoo: foo_value()=%d\n", v);
    return v == 42 ? 0 : 1;
}
