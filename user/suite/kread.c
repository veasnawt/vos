/* Read a present, supervisor-only kernel page: #PF P=1 U=1. */
#include "fixture.h"

int main(void)
{
    (void)*(volatile unsigned long *)KERNEL_TARGET;
    not_reached();
}
