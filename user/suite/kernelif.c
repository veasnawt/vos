/*
 * N2.1: ask the kernel (test-only call) whether RFLAGS.IF is set while it
 * handles a system call from this IF = 1 program. Exit status = the answer
 * (expected 0).
 */
#include "fixture.h"

#define SYS_TEST_KERNEL_IF 0x7E1F

int main(void)
{
    return (int)raw_syscall(SYS_TEST_KERNEL_IF, 0, 0);
}
