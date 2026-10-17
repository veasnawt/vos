/*
 * N2.1: try to clear IF and raise IOPL to 3 with popfq. At CPL 3 with IOPL 0
 * the CPU ignores both changes silently. Exit 1 if IF became 0, 2 if IOPL
 * changed; otherwise spin, so that only timer interrupts (and the kernel's
 * test tick budget) can end the program.
 */
#include "fixture.h"

int main(void)
{
    unsigned long f;

    __asm__ volatile ("pushfq\n\t"
                      "andq $~0x200, (%%rsp)\n\t"
                      "orq $0x3000, (%%rsp)\n\t"
                      "popfq\n\t"
                      "pushfq\n\t"
                      "popq %0" : "=r"(f) : : "memory", "cc");
    if (!(f & 0x200))
        return 1;
    if (f & 0x3000)
        return 2;
    for (;;)
        __asm__ volatile ("" : : : "memory");
}
