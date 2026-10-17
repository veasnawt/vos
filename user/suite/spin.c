/*
 * N2.1: a CPU-bound endless loop that never makes a system call. Only the
 * kernel can end it (in test builds, with its tick budget); its expected
 * outcome is in the trusted test configuration.
 */
#include "fixture.h"

int main(void)
{
    for (;;)
        __asm__ volatile ("" : : : "memory");
}
