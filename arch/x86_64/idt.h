/* Interrupt descriptor table for CPU exceptions (vectors 0-31). */
#ifndef VOS_X86_64_IDT_H
#define VOS_X86_64_IDT_H

#include <stdint.h>

#define IDT_VECTORS     32              /* CPU exception vectors */
#define IDT_ENTRIES     256             /* the IDT covers every vector; unused gates are absent */
#define IDT_SYSCALL     0x80            /* provisional system-call gate, DPL 3 */

/* Build and load the IDT. External interrupts stay disabled. */
void idt_init(void);
/* The IST index of a gate (0 = none). */
unsigned idt_gate_ist(unsigned vector);
#ifdef VOS_TEST
/* Test support: mark a gate not-present. */
void idt_clear_gate(unsigned vector);
#endif

#endif
