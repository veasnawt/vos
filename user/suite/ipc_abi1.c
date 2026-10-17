/*
 * N2.4: the smallest ABI v1 executable (linked with abi_note_v1.o and
 * syscall_v1.o). It yields once and exits with the result of vos_yield
 * (0 when the kernel implements ABI v1 for it).
 */
#include "fixture.h"

int main(void)
{
    return (int)-vos_yield();
}
