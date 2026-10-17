/* Interrupt descriptor table for CPU exceptions (vectors 0-31). */
#ifndef VOS_X86_64_IDT_H
#define VOS_X86_64_IDT_H

#include <stdint.h>

#define IDT_VECTORS 32

/* Build and load the IDT. External interrupts stay disabled. */
void idt_init(void);
#ifdef VOS_TEST
/* Test support: mark a gate not-present. */
void idt_clear_gate(unsigned vector);
#endif

#endif
