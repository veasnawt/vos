/* x86_64 CPU primitives: port I/O, MSRs, interrupt flag and halt. */
#ifndef VOS_X86_64_CPU_H
#define VOS_X86_64_CPU_H

#include <stdint.h>

#define MSR_EFER        0xC0000080
#define EFER_SCE        (1ULL << 0)
#define EFER_LME        (1ULL << 8)
#define EFER_LMA        (1ULL << 10)
#define EFER_NXE        (1ULL << 11)

#define CR0_PE          (1ULL << 0)
#define CR0_MP          (1ULL << 1)
#define CR0_EM          (1ULL << 2)
#define CR0_TS          (1ULL << 3)
#define CR0_NE          (1ULL << 5)
#define CR0_WP          (1ULL << 16)
#define CR0_AM          (1ULL << 18)
#define CR0_NW          (1ULL << 29)
#define CR0_CD          (1ULL << 30)
#define CR0_PG          (1ULL << 31)

#define CR4_VME         (1ULL << 0)
#define CR4_PVI         (1ULL << 1)
#define CR4_TSD         (1ULL << 2)
#define CR4_DE          (1ULL << 3)
#define CR4_PSE         (1ULL << 4)
#define CR4_PAE         (1ULL << 5)
#define CR4_MCE         (1ULL << 6)
#define CR4_PGE         (1ULL << 7)
#define CR4_PCE         (1ULL << 8)
#define CR4_OSFXSR      (1ULL << 9)
#define CR4_OSXMMEXCPT  (1ULL << 10)
#define CR4_UMIP        (1ULL << 11)
#define CR4_FSGSBASE    (1ULL << 16)
#define CR4_PCIDE       (1ULL << 17)
#define CR4_OSXSAVE     (1ULL << 18)
#define CR4_SMEP        (1ULL << 20)
#define CR4_SMAP        (1ULL << 21)
#define CR4_PKE         (1ULL << 22)
#define CR4_CET         (1ULL << 23)
#define CR4_PKS         (1ULL << 24)

/*
 * U1: the CR4 bits the kernel decides (set or clear) whatever the loader
 * left; every other bit (LA57, VMXE, SMXE, reserved) is preserved as found.
 */
#define CR4_MANAGED     (CR4_VME | CR4_PVI | CR4_TSD | CR4_DE | CR4_PSE | CR4_MCE | CR4_PGE | CR4_PCE | \
                         CR4_OSFXSR | CR4_OSXMMEXCPT | CR4_UMIP | CR4_FSGSBASE | CR4_PCIDE | CR4_OSXSAVE | \
                         CR4_SMEP | CR4_SMAP | CR4_PKE | CR4_CET | CR4_PKS)

/* The control registers as the loader left them (after boot.S set PAE, LME and PG). */
struct cpu_inherited {
    uint64_t cr0, cr4, efer;
};
extern struct cpu_inherited cpu_inherited;

/* 1 once CR4.SMAP is set; the entry stub then executes clac (entry.S). */
extern uint8_t cpu_smap_enabled;

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

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
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

#define RFLAGS_IF       (1ULL << 9)
#define RFLAGS_IOPL     (3ULL << 12)

static inline uint64_t read_rflags(void)
{
    uint64_t v;
    __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(v));
    return v;
}

static inline void cpu_disable_interrupts(void)
{
    __asm__ volatile ("cli" : : : "memory");
}

static inline void cpu_halt(void)
{
    __asm__ volatile ("hlt");
}

/*
 * Put CR0, CR4 and EFER into the kernel's defined state, whatever the loader
 * left (BIOS GRUB and UEFI GRUB differ, U0), and enable the protections:
 * CR0.WP and EFER.NXE (required), SMEP/UMIP/SMAP (when present), CR0.EM so
 * x87 raises #NM and MMX/SSE raise #UD (ABI v0: no FPU). See cpu.c for each bit.
 */
void cpu_init_protections(void);
/* NULL if CR0, CR4 and EFER are in the state cpu_init_protections set, else the first difference. */
const char *cpu_control_audit(void);

#endif
