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
 * IF policy: the kernel runs with IF = 0; ring 3 runs with IF = 1 (entry
 * RFLAGS 0x202). The one exception is the idle thread's "sti; hlt" (N2.2):
 * a hardware interrupt taken there is acknowledged and returns to the idle
 * loop, which immediately executes cli. A hardware interrupt from ring 3 is
 * handled on the TSS.RSP0 stack, acknowledged, and the program resumes. Any
 * other IRQ vector seen from ring 0 is a kernel error (panic). irq_init
 * unmasks only IRQ0.
 *
 * Device lines (N3.4). A driver registers a handler for its line
 * (irq_register): the line must not be the timer or the cascade, and its
 * trigger mode in the chipset's ELCR must be the one the device needs
 * (PCI INTx: level). Registration unmasks the line, and the cascade (IRQ2)
 * for a slave line; the last unregistration masks them again. Up to
 * IRQ_MAX_HANDLERS handlers exist in total, and lines may be shared: every
 * handler of the line is asked, in registration order.
 *
 * A handler has two halves. The top half runs inside irq_handle (IF = 0,
 * nothing nested, before the EOI): it must be short, never block, sleep,
 * yield, allocate or wake threads (the scheduler panics if called), and
 * must acknowledge its device so a level-triggered line drops before the
 * EOI; it returns IRQ_HANDLED or IRQ_NONE (not my device). The bottom half
 * of every handler that said IRQ_HANDLED runs later in thread context,
 * from irq_run_deferred, which the scheduler calls after each timer tick
 * from ring 3 (sched_tick) and in the idle loop after each halt; it may
 * wake threads. Top halves only ever interrupt ring 3 or the idle thread's
 * halt, never kernel code, so they never interleave with a driver's own
 * register accesses (PCI configuration cycles included).
 *
 * Storms. A device line that interrupts more than IRQ_STORM_LIMIT times
 * within one timer tick (claimed or not: a device that never lets go of a
 * level line, or a foreign device nobody acknowledges) is masked for good;
 * its handlers' bottom halves run once more so their drivers notice
 * (irq_line_disabled) and fall back to polling.
 */
#ifndef VOS_X86_64_IRQ_H
#define VOS_X86_64_IRQ_H

#include <stdint.h>

#define IRQ_VECTOR_BASE     0x20
#define IRQ_LINES           16
#define IRQ_TIMER           0
#define IRQ_MAX_HANDLERS    4
#define IRQ_STORM_LIMIT     1000        /* device-line interrupts within one timer tick before the line is masked */

/* Trigger modes (irq_register). */
#define IRQ_TRIGGER_EDGE    0
#define IRQ_TRIGGER_LEVEL   1

/* Results of a top half. */
#define IRQ_NONE            0   /* not from this device */
#define IRQ_HANDLED         1   /* from this device, acknowledged at the device: run the bottom half */

/* Results of irq_register / irq_unregister. */
#define IRQ_OK              0
#define IRQ_EINVAL          1   /* the timer or cascade line, no such line, no top half, bad handle */
#define IRQ_EFULL           2   /* no free handler record */
#define IRQ_ETRIGGER        3   /* the line's trigger mode (ELCR) is not the one the device needs */
#define IRQ_EDISABLED       4   /* the line was masked after a storm */

typedef int (*irq_top_fn)(void *ctx);
typedef void (*irq_bottom_fn)(void *ctx);

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
    uint16_t (*triggers)(void);             /* bit n set = line n level triggered */
};

struct irq_stats {
    uint64_t delivered[IRQ_LINES];          /* genuine, acknowledged */
    uint64_t spurious[IRQ_LINES];
    uint64_t unexpected;                    /* genuine on a line that should be masked */
    /* N3.4: device lines. */
    uint64_t claimed[IRQ_LINES];            /* at least one top half said IRQ_HANDLED */
    uint64_t unclaimed[IRQ_LINES];          /* every top half of the line said IRQ_NONE */
    uint64_t storms;                        /* lines masked after a storm */
    uint64_t bottom_runs;                   /* bottom halves run by irq_run_deferred */
};

/*
 * Set up the controller (all lines masked, vectors 0x20-0x2F), program the
 * PIT, then unmask IRQ0. The IDT gates must already exist. IF stays 0.
 */
void irq_init(void);
/*
 * Handle one hardware interrupt taken from ring 3, or by the idle thread in
 * its "sti; hlt" (N2.2, see thread.h) (frame vector 0x20-0x2F):
 * classify it at the controller, count it, acknowledge it before returning
 * and check that it left service. Panics for a vector that is not in service
 * at the controller, for one still in service after EOI, for nesting and if
 * IF is set.
 */
struct exc_frame;
void irq_handle(struct exc_frame *f);
uint64_t irq_ticks(void);                   /* timer interrupts handled since irq_init */
int irq_in_handler(void);                   /* nonzero inside irq_handle */
const struct irq_stats *irq_stats(void);
const struct irq_chip *irq_chip(void);

/*
 * Register a handler for device line `line` (thread context, IF = 0).
 * *handle receives its handle (> 0). IRQ_OK or IRQ_E*. bottom may be NULL.
 */
int irq_register(unsigned line, int trigger, const char *name, irq_top_fn top, irq_bottom_fn bottom, void *ctx,
                 int *handle);
/* Remove a handler; a pending bottom half is dropped. The line is masked when its last handler goes. */
int irq_unregister(int handle);
/* Run the bottom halves whose top half claimed an interrupt (thread context, IF = 0; never in a handler). */
void irq_run_deferred(void);
/* The masks the controller must hold: IRQ0, the registered lines not disabled, and the cascade if needed. */
uint16_t irq_expected_masks(void);
/* Nonzero if the line was masked after a storm. */
int irq_line_disabled(unsigned line);
/* Consistency of the handler table and the controller state; NULL if consistent. */
const char *irq_audit(void);

#ifdef VOS_TEST
/* Run line `line`'s top halves (and the storm guard) as an interrupt would; returns how many claimed it. */
unsigned irq_test_dispatch(unsigned line);
/* Forget that line `line` was disabled by a storm (it stays masked until a handler registers). */
void irq_test_clear_storm(unsigned line);
#endif

#endif
