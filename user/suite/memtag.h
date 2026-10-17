/*
 * N2.3 memory-isolation fixture body: keep writing TAG to a page of this
 * program's own .bss and check, after a short spin, that it is still there.
 * Exit 1 if the page ever holds anything else (another process's writes
 * reached it). Otherwise it never ends: the kernel kills it. memtag1.c and
 * memtag2.c differ only in TAG and are loaded at the same addresses.
 */
#include "fixture.h"

static volatile unsigned long page[512] __attribute__((aligned(4096)));

int main(void)
{
    for (;;) {
        page[0] = TAG;
        for (volatile int i = 0; i < 20000; i++)
            ;
        if (page[0] != TAG)
            return 1;
    }
}
