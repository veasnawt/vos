/* Control-register access: #GP(0). */
#include "fixture.h"

int main(void)
{
    unsigned long v;

    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    not_reached();
}
