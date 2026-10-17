/*
 * Set RFLAGS.AC (allowed at CPL 3), then ask the kernel (test-only call
 * 0x7E5A) whether AC is set while it runs. With SMAP the entry path clears it,
 * so the answer is 0 and the program exits 0; without SMAP it stays set (1).
 * A normal debug_write must still work with AC set.
 */
#include "fixture.h"

static const char msg[] = "acflag: write with AC set\n";

int main(void)
{
    __asm__ volatile ("pushfq\n\torq $0x40000, (%%rsp)\n\tpopfq" : : : "memory", "cc");
    if (vos_debug_write(msg, sizeof(msg) - 1) != (long)(sizeof(msg) - 1))
        return 2;
    return raw_syscall(0x7E5A, 0, 0) != 0;
}
