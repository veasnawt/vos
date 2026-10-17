/* Read address 0 (null guard): #PF U=1, CR2 0. */
#include "fixture.h"

int main(void)
{
    __asm__ volatile ("mov (%0), %%rax" : : "r"(0UL) : "rax", "memory");
    not_reached();
}
