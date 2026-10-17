/* Write a present, supervisor-only kernel page: #PF P=1 W=1 U=1. */
#include "fixture.h"

int main(void)
{
    *(volatile unsigned long *)KERNEL_TARGET = 0;
    not_reached();
}
