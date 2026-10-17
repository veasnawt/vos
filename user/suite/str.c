/* With CR4.UMIP: #GP(0); without it, str works and the program exits 0. */
#include "fixture.h"

int main(void)
{
    unsigned long v;

    __asm__ volatile ("str %0" : "=r"(v));
    return 0;
}
