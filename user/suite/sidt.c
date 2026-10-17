/* With CR4.UMIP: #GP(0); without it, sidt works and the program exits 0. */
#include "fixture.h"

int main(void)
{
    volatile unsigned char d[10];

    __asm__ volatile ("sidt %0" : "=m"(d));
    return 0;
}
