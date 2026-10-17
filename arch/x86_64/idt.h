/* Interrupt descriptor table: CPU exceptions, IRQ0-15 (0x20-0x2F, see irq.h) and int $0x80. */
#ifndef VOS_X86_64_IDT_H
#define VOS_X86_64_IDT_H

#include <stdint.h>

#define IDT_VECTORS     32              /* CPU exception vectors */
#define IDT_ENTRIES     256             /* the IDT covers every vector; unused gates are absent */
#define IDT_SYSCALL     0x80            /* provisional system-call gate, DPL 3 */

/* Build and load the IDT. The kernel keeps IF = 0; see irq.h for the IF policy. */
void idt_init(void);
/* The IST index of a gate (0 = none), its type/attribute byte (0 = absent) and its handler. */
unsigned idt_gate_ist(unsigned vector);
unsigned idt_gate_type(unsigned vector);
uint64_t idt_gate_handler(unsigned vector);
#ifdef VOS_TEST
/* Test support: mark a gate not-present. */
void idt_clear_gate(unsigned vector);
#endif

#endif
