/* With CR4.UMIP: #GP(0); without it, sgdt works and the program exits 0. */
#include "fixture.h"

int main(void)
{
    volatile unsigned char d[10];

    __asm__ volatile ("sgdt %0" : "=m"(d));
    return 0;
}
