/* Permanent kernel GDT and TSS. */
#ifndef VOS_X86_64_GDT_H
#define VOS_X86_64_GDT_H

#include <stdint.h>

#define GDT_KERNEL_CS   0x08
#define GDT_KERNEL_DS   0x10
#define GDT_USER_DS     0x1B    /* DPL 3; prepared for later stages, unused in N1.1 */
#define GDT_USER_CS     0x23
#define GDT_TSS         0x28

#define IST_DOUBLE_FAULT    1
#define IST_NMI             2
#define IST_MACHINE_CHECK   3

/* Load the GDT, reload the segment registers and load the task register. */
void gdt_init(void);
/* Virtual addresses of the TSS and of its RSP0 and IST1 stack tops. */
uint64_t gdt_tss_base(void);
uint64_t gdt_tss_rsp0(void);
uint64_t gdt_tss_ist1(void);
/* Top of IST stack n (1..3), or 0. */
uint64_t gdt_tss_ist(unsigned n);
/* Virtual addresses of the guard pages below the RSP0 and IST stacks; returns their number. */
unsigned gdt_guard_pages(uint64_t *va, unsigned max);
/* "entry", "ist1", "ist2" or "ist3" if addr lies in that stack, else NULL. */
const char *gdt_stack_name(uint64_t addr);
/* Nonzero if the canaries at the bottom of the RSP0 and all IST stacks are intact. */
int gdt_stack_canaries_intact(void);
#ifdef VOS_TEST
/* Test hook: overwrite (1) or restore (0) one RSP0 stack canary word. */
void gdt_test_smash_canary(int smash);
#endif
/* Nonzero if addr lies inside the double-fault IST stack. */
int gdt_in_ist1_stack(uint64_t addr);

#endif
