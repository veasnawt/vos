/*
 * N2.4: an ABI v0 program (v0 note) calling the ABI v1 system calls 2-5.
 * Each must return -VOS_ENOSYS. Exit 0, or the first number that did not.
 */
#include "fixture.h"

int main(void)
{
    for (long n = 2; n <= 5; n++)
        if (raw_syscall(n, 1, 0) != -VOS_ENOSYS)
            return (int)n;
    return 0;
}
