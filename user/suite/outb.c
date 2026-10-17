/* Port output to COM1: #GP(0). The byte 0x01 must never reach the serial log. */
#include "fixture.h"

int main(void)
{
    __asm__ volatile ("outb %%al, %%dx" : : "a"(0x01), "d"(0x3F8));
    not_reached();
}
