/* Permanent kernel GDT and TSS. */
#ifndef VOS_X86_64_GDT_H
#define VOS_X86_64_GDT_H

#include <stdint.h>

#define GDT_KERNEL_CS   0x08
#define GDT_KERNEL_DS   0x10
#define GDT_USER_DS     0x1B    /* DPL 3; prepared for later stages, unused in N1.1 */
#define GDT_USER_CS     0x23
#define GDT_TSS         0x28

#define IST_DOUBLE_FAULT 1

/* Load the GDT, reload the segment registers and load the task register. */
void gdt_init(void);
/* Virtual addresses of the TSS and of its RSP0 and IST1 stack tops. */
uint64_t gdt_tss_base(void);
uint64_t gdt_tss_rsp0(void);
uint64_t gdt_tss_ist1(void);
/* Nonzero if the canaries at the bottom of the RSP0 and IST1 stacks are intact. */
int gdt_stack_canaries_intact(void);
/* Nonzero if addr lies inside the double-fault IST stack. */
int gdt_in_ist1_stack(uint64_t addr);

#endif
