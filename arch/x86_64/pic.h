/*
 * Legacy 8259A interrupt controller pair (master at 0x20/0x21, slave at
 * 0xA0/0xA1, slave cascaded on master IRQ2), as emulated by QEMU's pc machine.
 *
 * Every function takes the port-I/O backend explicitly, so the self-tests can
 * run the same code against a recording fake (synthetic unit tests) as well
 * as against the real chips.
 *
 * Spurious interrupts: the 8259 reports IRQ7 (master) or IRQ15 (slave) when
 * the request that started an acknowledge cycle has gone by the time the CPU
 * asks for the vector. Such an interrupt has no in-service bit. A spurious
 * IRQ7 gets no EOI at all; a spurious IRQ15 gets an EOI for the cascade line
 * on the master only (the master did see a real request on IRQ2).
 */
#ifndef VOS_X86_64_PIC_H
#define VOS_X86_64_PIC_H

#include <stdint.h>

#define PIC_MASTER_CMD      0x20
#define PIC_MASTER_DATA     0x21
#define PIC_SLAVE_CMD       0xA0
#define PIC_SLAVE_DATA      0xA1

#define PIC_ICW1_INIT       0x11    /* init, edge triggered, cascade, ICW4 follows */
#define PIC_ICW3_MASTER     0x04    /* slave attached to IRQ2 (bit mask) */
#define PIC_ICW3_SLAVE      0x02    /* slave identity: cascade line 2 */
#define PIC_ICW4_8086       0x01    /* 8086 mode, normal (non-automatic) EOI, not buffered */
#define PIC_OCW2_EOI        0x20    /* non-specific EOI */
#define PIC_OCW3_READ_IRR   0x0A
#define PIC_OCW3_READ_ISR   0x0B
#define PIC_CASCADE_IRQ     2

struct pic_io {
    uint8_t (*in)(uint16_t port);
    void (*out)(uint16_t port, uint8_t value);
};

/* The real chips (port I/O, with a short delay write to port 0x80 after each command). */
extern const struct pic_io pic_port_io;

/* Results of pic_begin. */
#define PIC_GENUINE         0       /* in service: handle it, then pic_eoi */
#define PIC_SPURIOUS        1       /* IRQ7/15 without in-service bit: do not handle; acknowledged as needed */
#define PIC_NOT_IN_SERVICE  2       /* any other line without in-service bit: not from the PIC */

/*
 * Reinitialize both chips: master vectors base..base+7, slave base+8..base+15
 * (base must be a multiple of 8), every line masked, normal EOI mode.
 */
void pic_init(const struct pic_io *io, uint8_t base);
void pic_set_mask(const struct pic_io *io, unsigned irq, int masked);
uint16_t pic_masks(const struct pic_io *io);    /* slave IMR << 8 | master IMR */
uint16_t pic_isr(const struct pic_io *io);      /* in-service registers, same layout */
uint16_t pic_irr(const struct pic_io *io);      /* request registers, same layout */
/*
 * Classify a delivered IRQ line (0..15) by the in-service registers: a line
 * from the slave also needs the master's cascade bit. See above for 7 and 15.
 */
int pic_begin(const struct pic_io *io, unsigned irq);
/* End of interrupt for a genuine IRQ: the slave and the master for 8..15, the master for 0..7. */
void pic_eoi(const struct pic_io *io, unsigned irq);

#endif
