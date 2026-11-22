/* Stretch case: a normal glibc program, built with the host gcc. */
#include <stdio.h>

int main(void)
{
    printf("glibc %d %s %lx\n", 42, "ok", 0xdeadbeefUL);
    return 0;
}
