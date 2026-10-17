/* Fill 64 KiB of .bss and 12 KiB of stack with 0x5A, then exit 0 (followed by "clean"). */
#include "fixture.h"

static volatile unsigned char big[65536];

int main(void)
{
    volatile unsigned char stack[12288];

    for (unsigned long i = 0; i < sizeof(big); i++)
        big[i] = 0x5A;
    for (unsigned long i = 0; i < sizeof(stack); i++)
        stack[i] = 0x5A;
    return 0;
}
