/* x86_64 CPU primitives: port I/O, MSRs, interrupt flag and halt. */
#ifndef VOS_X86_64_CPU_H
#define VOS_X86_64_CPU_H

#include <stdint.h>

#define MSR_EFER        0xC0000080
#define EFER_LMA        (1ULL << 10)

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpu_disable_interrupts(void)
{
    __asm__ volatile ("cli" : : : "memory");
}

static inline void cpu_halt(void)
{
    __asm__ volatile ("hlt");
}

#endif
