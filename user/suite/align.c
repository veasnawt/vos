/* psABI stack alignment through crt0 (plan 8.3, U12): exit 0 if aligned everywhere. */
#include "fixture.h"

extern unsigned long __vos_entry_rsp;

static unsigned long addr(volatile char *p)
{
    unsigned long v = (unsigned long)p;

    __asm__ ("" : "+r"(v));             /* hide the address from the optimizer */
    return v;
}

__attribute__((noinline)) static int nested(void)
{
    volatile char a[32] __attribute__((aligned(16)));

    a[0] = 1;
    return (addr(a) & 15) != 0;
}

int main(void)
{
    volatile char a[32] __attribute__((aligned(16)));

    a[0] = 1;
    if (addr(a) & 15)
        return 1;
    if (nested())
        return 2;
    if (__vos_entry_rsp != 0x7FFFFFFFEFF0UL)
        return 3;
    return 0;
}
