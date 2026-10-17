/* Port input: #GP(0). */
#include "fixture.h"

int main(void)
{
    unsigned char v;

    __asm__ volatile ("inb %%dx, %%al" : "=a"(v) : "d"(0x60));
    not_reached();
}
