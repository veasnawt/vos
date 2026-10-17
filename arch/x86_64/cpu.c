#include "cpu.h"
#include "console.h"
#include "panic.h"

#define CPUID_EXT_NX        (1u << 20)  /* CPUID 0x80000001 EDX */
#define CPUID7_EBX_SMEP     (1u << 7)   /* CPUID 7.0 EBX */
#define CPUID7_EBX_SMAP     (1u << 20)
#define CPUID7_ECX_UMIP     (1u << 2)   /* CPUID 7.0 ECX */

/*
 * Enable the protections the kernel mappings allow. NX is mandatory. SMEP and
 * UMIP are enabled when present: every mapping is supervisor-only, so neither
 * can fault kernel code. SMAP stays off until user-memory access exists.
 */
void cpu_init_protections(void)
{
    uint32_t a, b, c, d, max_basic, max_ext, smep = 0, umip = 0, smap = 0;

    cpuid(0x80000000, 0, &max_ext, &b, &c, &d);
    if (max_ext < 0x80000001)
        panic("CPU lacks NX");
    cpuid(0x80000001, 0, &a, &b, &c, &d);
    if (!(d & CPUID_EXT_NX))
        panic("CPU lacks NX");

    cpuid(0, 0, &max_basic, &b, &c, &d);
    if (max_basic >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        smep = b & CPUID7_EBX_SMEP;
        smap = b & CPUID7_EBX_SMAP;
        umip = c & CPUID7_ECX_UMIP;
    }

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
    /*
     * EM = 1 (MP = 0, TS = 0): no x87 for anyone. The kernel is built with
     * -mgeneral-regs-only and contains no x87/MMX/SSE instruction, so only
     * user code is affected: x87 -> #NM, MMX/SSE -> #UD (Intel SDM 2.5).
     */
    write_cr0((read_cr0() | CR0_WP | CR0_EM) & ~(CR0_MP | CR0_TS));
    uint64_t cr4 = read_cr4();
    if (smep)
        cr4 |= CR4_SMEP;
    if (umip)
        cr4 |= CR4_UMIP;
    write_cr4(cr4);

    if (!(rdmsr(MSR_EFER) & EFER_NXE) || !(read_cr0() & CR0_WP) || !(read_cr0() & CR0_EM) ||
        (read_cr0() & (CR0_MP | CR0_TS)) ||
        (smep && !(read_cr4() & CR4_SMEP)) || (umip && !(read_cr4() & CR4_UMIP)))
        panic("CPU protection bits did not stick");

    serial_puts("cpu: wp=on nx=on smep=");
    serial_puts(smep ? "on" : "unsupported");
    serial_puts(" umip=");
    serial_puts(umip ? "on" : "unsupported");
    serial_puts(smap ? " smap=disabled(deferred)" : " smap=unsupported");
    serial_puts(" x87=off(em)");
    serial_puts("\r\ncpu: cr0=");
    serial_hex(read_cr0());
    serial_puts(" cr4=");
    serial_hex(read_cr4());
    serial_puts(" efer=");
    serial_hex(rdmsr(MSR_EFER));
    serial_puts("\r\n");
}
