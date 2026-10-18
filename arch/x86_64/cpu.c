#include <stddef.h>

#include "cpu.h"
#include "console.h"
#include "panic.h"

#define CPUID_EXT_NX        (1u << 20)  /* CPUID 0x80000001 EDX */
#define CPUID7_EBX_SMEP     (1u << 7)   /* CPUID 7.0 EBX */
#define CPUID7_EBX_SMAP     (1u << 20)
#define CPUID7_ECX_UMIP     (1u << 2)   /* CPUID 7.0 ECX */

#define CPUID1_EDX_DE       (1u << 2)   /* CPUID 1 EDX: debugging extensions */
#define CPUID1_EDX_MCE      (1u << 7)   /* machine-check exception */

uint8_t cpu_smap_enabled;
struct cpu_inherited cpu_inherited;
static uint64_t want_cr0, want_cr4;     /* the state cpu_init_protections established */

/*
 * U1: the defined control state. Required bits are asserted, never written
 * off; "managed" bits are set or cleared by this policy, so BIOS and UEFI
 * boots run identically; all other bits are preserved as the loader left them.
 *
 * CR0 (U0: BIOS GRUB leaves 0x11, UEFI GRUB 0x10033 at entry)
 *   PE, PG    required (long mode); never changed here
 *   WP        set: supervisor writes honour read-only pages (W^X)
 *   EM        set, MP and TS clear: no x87/MMX/SSE for anyone (ABI v0)
 *   NE        set: native x87 error reporting (#MF) instead of the legacy
 *             FERR#/IRQ13 path; UEFI already sets it; no effect while EM = 1
 *   AM        clear: no alignment checking for ring 3 (BIOS and OVMF leave 0)
 *   CD, NW    clear (normal caching); if a loader left them set, the caches
 *             are written back first (wbinvd)
 *   ET and any other bit: preserved
 * CR4 (U0: BIOS 0, UEFI 0x648 = DE, MCE, OSFXSR, OSXMMEXCPT; boot.S adds PAE)
 *   PAE       required; never changed here
 *   DE        set when CPUID reports it: DR4/DR5 raise #UD (no legacy aliases)
 *   MCE       set when CPUID reports it: a machine check raises #MC (a panic
 *             with a message) instead of shutting the machine down; BIOS boots
 *             change from 0 to 1 here
 *   SMEP, SMAP, UMIP  set when CPUID reports them (unchanged policy)
 *   cleared:  VME, PVI (no virtual-8086), TSD (RDTSC stays allowed in ring 3,
 *             as before on both paths), PSE (ignored in long mode), PGE (no
 *             global pages; vmm.c and kmap.c flush correctly either way), PCE
 *             (no RDPMC in ring 3), OSFXSR, OSXMMEXCPT, OSXSAVE (VOS saves no
 *             SSE/AVX state), FSGSBASE (no WRFSBASE in ring 3), PCIDE (no
 *             PCIDs), PKE, PKS, CET (unused features that would change access
 *             rules)
 *   preserved: LA57, VMXE, SMXE and reserved bits
 * EFER
 *   LME, LMA  required; NXE set (required)
 *   SCE       cleared: SYSCALL is not configured (IA32_LSTAR holds whatever the
 *             firmware left), so a ring-3 SYSCALL must raise #UD
 *   any other bit: preserved
 */

/*
 * Enable the protections the kernel mappings allow. NX is mandatory. SMEP and
 * UMIP are enabled when present: the kernel never executes or describes user
 * pages. SMAP is enabled when present (N1.9): the kernel never accesses a
 * user page through a user address (copy_from_user reads through the
 * supervisor-only kernel alias), and every kernel entry executes clac, so a
 * user-set RFLAGS.AC cannot open user pages to the kernel.
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

    cpuid(1, 0, &a, &b, &c, &d);
    uint32_t de = d & CPUID1_EDX_DE, mce = d & CPUID1_EDX_MCE;

    cpu_inherited.cr0 = read_cr0();
    cpu_inherited.cr4 = read_cr4();
    cpu_inherited.efer = rdmsr(MSR_EFER);
    if ((cpu_inherited.cr0 & (CR0_PE | CR0_PG)) != (CR0_PE | CR0_PG) || !(cpu_inherited.cr4 & CR4_PAE) ||
        (cpu_inherited.efer & (EFER_LME | EFER_LMA)) != (EFER_LME | EFER_LMA))
        panic("CPU not in long mode with paging (CR0.PE/PG, CR4.PAE, EFER.LME/LMA)");

    wrmsr(MSR_EFER, (cpu_inherited.efer | EFER_NXE) & ~EFER_SCE);
    /*
     * EM = 1 (MP = 0, TS = 0): no x87 for anyone. The kernel is built with
     * -mgeneral-regs-only and contains no x87/MMX/SSE instruction, so only
     * user code is affected: x87 -> #NM, MMX/SSE -> #UD (Intel SDM 2.5).
     */
    if (cpu_inherited.cr0 & (CR0_CD | CR0_NW))
        __asm__ volatile ("wbinvd" : : : "memory");
    want_cr0 = (cpu_inherited.cr0 | CR0_WP | CR0_EM | CR0_NE) & ~(CR0_MP | CR0_TS | CR0_AM | CR0_CD | CR0_NW);
    write_cr0(want_cr0);
    want_cr4 = (cpu_inherited.cr4 & ~CR4_MANAGED) | (de ? CR4_DE : 0) | (mce ? CR4_MCE : 0) |
               (smep ? CR4_SMEP : 0) | (umip ? CR4_UMIP : 0) | (smap ? CR4_SMAP : 0);
    write_cr4(want_cr4);

    const char *bad = cpu_control_audit();
    if (bad) {
        serial_puts("cpu: ");
        serial_puts(bad);
        serial_puts("\r\n");
        panic("CPU control state did not stick");
    }

    serial_puts("cpu: wp=on nx=on smep=");
    serial_puts(smep ? "on" : "unsupported");
    serial_puts(" umip=");
    serial_puts(umip ? "on" : "unsupported");
    cpu_smap_enabled = smap ? 1 : 0;
    serial_puts(smap ? " smap=on" : " smap=unsupported");
    serial_puts(" x87=off(em)");
    serial_puts("\r\ncpu: inherited cr0=");
    serial_hex(cpu_inherited.cr0);
    serial_puts(" cr4=");
    serial_hex(cpu_inherited.cr4);
    serial_puts(" efer=");
    serial_hex(cpu_inherited.efer);
    serial_puts("; set cr0=");
    serial_hex(read_cr0());
    serial_puts(" cr4=");
    serial_hex(read_cr4());
    serial_puts(" efer=");
    serial_hex(rdmsr(MSR_EFER));
    serial_puts("\r\n");
}

const char *cpu_control_audit(void)
{
    uint64_t cr0 = read_cr0(), cr4 = read_cr4(), efer = rdmsr(MSR_EFER);

    if (!want_cr0)
        return "control state not initialized";
    if (cr0 != want_cr0)
        return "CR0 differs from the defined state";
    if (cr4 != want_cr4)
        return "CR4 differs from the defined state";
    if ((efer & (EFER_LME | EFER_LMA | EFER_NXE | EFER_SCE)) != (EFER_LME | EFER_LMA | EFER_NXE))
        return "EFER: LME, LMA, NXE must be set and SCE clear";
    if ((cr0 & (CR0_PE | CR0_PG | CR0_WP | CR0_EM | CR0_NE)) != (CR0_PE | CR0_PG | CR0_WP | CR0_EM | CR0_NE) ||
        (cr0 & (CR0_MP | CR0_TS | CR0_AM | CR0_CD | CR0_NW)))
        return "CR0 policy bits wrong";
    if (!(cr4 & CR4_PAE) || (cr4 & (CR4_VME | CR4_PVI | CR4_TSD | CR4_PSE | CR4_PGE | CR4_PCE | CR4_OSFXSR |
                                    CR4_OSXMMEXCPT | CR4_FSGSBASE | CR4_PCIDE | CR4_OSXSAVE | CR4_PKE | CR4_CET |
                                    CR4_PKS)))
        return "CR4 policy bits wrong";
    return NULL;
}
