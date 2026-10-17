/* x86_64 CPU primitives: port I/O, MSRs, interrupt flag and halt. */
#ifndef VOS_X86_64_CPU_H
#define VOS_X86_64_CPU_H

#include <stdint.h>

#define MSR_EFER        0xC0000080
#define EFER_LMA        (1ULL << 10)
#define EFER_NXE        (1ULL << 11)

#define CR0_WP          (1ULL << 16)
#define CR4_UMIP        (1ULL << 11)
#define CR4_SMEP        (1ULL << 20)

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

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32))
                      : "memory");
}

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b,
                         uint32_t *c, uint32_t *d)
{
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static inline uint64_t read_cr0(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint64_t v)
{
    __asm__ volatile ("mov %0, %%cr0" : : "r"(v) : "memory");
}

static inline uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline void invlpg(uint64_t va)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
}

static inline uint64_t read_cr4(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v)
{
    __asm__ volatile ("mov %0, %%cr4" : : "r"(v) : "memory");
}

static inline void cpu_disable_interrupts(void)
{
    __asm__ volatile ("cli" : : : "memory");
}

static inline void cpu_halt(void)
{
    __asm__ volatile ("hlt");
}

/* Enable CR0.WP, EFER.NXE (required) and SMEP/UMIP (when present). */
void cpu_init_protections(void);

#endif
