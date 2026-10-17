/*
 * Enter the kernel with DF = 1 for debug_write. The kernel clears DF on entry,
 * so the copy runs forward and the exact text is printed; the user DF is
 * restored on return.
 */
#include "fixture.h"

static const char msg[] = "df: copied forward\n";

int main(void)
{
    long r;
    unsigned long flags;

    __asm__ volatile ("std\n\tint $0x80\n\tpushfq\n\tpopq %1\n\tcld"
                      : "=a"(r), "=r"(flags)
                      : "a"((long)VOS_SYS_DEBUG_WRITE), "D"(msg), "S"(sizeof(msg) - 1)
                      : "memory", "cc");
    if (r != (long)(sizeof(msg) - 1))
        return 1;
    return (flags & 0x400) ? 0 : 2;     /* DF still set after the call */
}
