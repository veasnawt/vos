/*
 * Run right after "dirty": the allocator hands out the frames dirty used, so
 * any byte not zeroed by the kernel shows up here. Exit 0 if .bss and the
 * stack below this frame (where dirty's 12 KiB array was) are all zero.
 */
#include "fixture.h"

static volatile unsigned char big[65536];

int main(void)
{
    unsigned long dirty = 0, sp;

    for (unsigned long i = 0; i < sizeof(big); i++)
        dirty += big[i] != 0;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(sp));
    for (volatile unsigned char *p = (volatile unsigned char *)0x7FFFFFFFB000UL;
         (unsigned long)p < sp - 128; p++)
        dirty += *p != 0;
    return dirty ? 1 : 0;
}
