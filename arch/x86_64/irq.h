/*
 * Hardware interrupts (N2.1): one interrupt controller behind struct
 * irq_chip (today the 8259 pair, pic.c), the PIT on IRQ0 (pit.c) and the
 * kernel tick counter.
 *
 * Vector allocation:
 *   0x00-0x1F  CPU exceptions (#DF on IST1, NMI on IST2, #MC on IST3)
 *   0x20-0x27  IRQ0-7  (master 8259); IRQ0 = PIT channel 0, IRQ7 may be spurious
 *   0x28-0x2F  IRQ8-15 (slave 8259, cascaded on IRQ2); IRQ15 may be spurious
 *   0x80       system call (DPL 3)
 *   all other  not present
 * All gates are DPL 0 interrupt gates (no IST) except 0x80 (DPL 3): ring 3
 * cannot raise an IRQ vector with "int n" (#GP), and the CPU clears IF on
 * every entry.
 *
 * IF policy: the kernel always runs with IF = 0 and never executes sti; only
 * ring 3 runs with IF = 1 (entry RFLAGS 0x202). A hardware interrupt can
 * therefore only interrupt ring 3; it is handled on the TSS.RSP0 stack,
 * acknowledged, and the program resumes. An IRQ vector seen from ring 0 is a
 * kernel error (panic). Only IRQ0 is unmasked; the cascade line stays masked
 * because no slave line is used.
 */
#ifndef VOS_X86_64_IRQ_H
#define VOS_X86_64_IRQ_H

#include <stdint.h>

#define IRQ_VECTOR_BASE     0x20
#define IRQ_LINES           16
#define IRQ_TIMER           0

static inline int irq_is_vector(uint64_t v)
{
    return v >= IRQ_VECTOR_BASE && v < IRQ_VECTOR_BASE + IRQ_LINES;
}

/* Results of irq_chip.begin. */
#define IRQ_GENUINE         0
#define IRQ_SPURIOUS        1
#define IRQ_NOT_IN_SERVICE  2

/* The interrupt controller interface, so a later controller can replace the 8259. */
struct irq_chip {
    const char *name;
    void (*init)(uint8_t vector_base);      /* every line masked afterwards */
    void (*mask)(unsigned irq);
    void (*unmask)(unsigned irq);
    uint16_t (*masks)(void);                /* bit n set = line n masked */
    uint16_t (*in_service)(void);
    int (*begin)(unsigned irq);             /* IRQ_GENUINE / IRQ_SPURIOUS / IRQ_NOT_IN_SERVICE */
    void (*eoi)(unsigned irq);              /* acknowledge a genuine IRQ */
};

struct irq_stats {
    uint64_t delivered[IRQ_LINES];          /* genuine, acknowledged */
    uint64_t spurious[IRQ_LINES];
    uint64_t unexpected;                    /* genuine on a line that should be masked */
};

/*
 * Set up the controller (all lines masked, vectors 0x20-0x2F), program the
 * PIT, then unmask IRQ0. The IDT gates must already exist. IF stays 0.
 */
void irq_init(void);
/*
 * Handle one hardware interrupt taken from ring 3 (frame vector 0x20-0x2F):
 * classify it at the controller, count it, acknowledge it before returning.
 * Panics for a vector that is not in service at the controller, for nesting
 * and if IF is set.
 */
struct exc_frame;
void irq_handle(struct exc_frame *f);
uint64_t irq_ticks(void);                   /* timer interrupts handled since irq_init */
const struct irq_stats *irq_stats(void);
const struct irq_chip *irq_chip(void);

#endif
