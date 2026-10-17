/*
 * Isolation-suite fixtures (N1.9): separately built VOS executables, loaded
 * through the same GRUB/ELF/process path as hello. Each fixture's expected
 * outcome is declared in the trusted suite configuration
 * (tests/x86_64/suite.grub.cfg), never by the fixture's own output.
 */
#ifndef VOS_SUITE_FIXTURE_H
#define VOS_SUITE_FIXTURE_H

#include <vos/syscall.h>

/*
 * Runs only if the instruction before it did not fault. Its successful
 * debug_write is counted by the kernel, so "no system call succeeded" proves
 * that nothing after the fault executed.
 */
static inline __attribute__((noreturn)) void not_reached(void)
{
    vos_debug_write("not reached\n", 12);
    vos_exit(77);
}

/* A raw system call (for numbers the runtime has no stub for). */
static inline long raw_syscall(long n, long a, long b)
{
    long r;

    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "memory");
    return r;
}

#define KERNEL_TARGET 0xFFFFFFFF80100000UL  /* kernel alias of phys 0x100000 (kernel image) */

#endif
