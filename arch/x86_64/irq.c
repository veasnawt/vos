#include "irq.h"
#include "console.h"
#include "cpu.h"
#include "exception.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"

static void i8259_init(uint8_t base)
{
    pic_init(&pic_port_io, base);
}

static void i8259_mask(unsigned irq)
{
    pic_set_mask(&pic_port_io, irq, 1);
}

static void i8259_unmask(unsigned irq)
{
    pic_set_mask(&pic_port_io, irq, 0);
}

static uint16_t i8259_masks(void)
{
    return pic_masks(&pic_port_io);
}

static uint16_t i8259_isr(void)
{
    return pic_isr(&pic_port_io);
}

static int i8259_begin(unsigned irq)
{
    int r = pic_begin(&pic_port_io, irq);

    return r == PIC_GENUINE ? IRQ_GENUINE : r == PIC_SPURIOUS ? IRQ_SPURIOUS : IRQ_NOT_IN_SERVICE;
}

static void i8259_eoi(unsigned irq)
{
    pic_eoi(&pic_port_io, irq);
}

static const struct irq_chip i8259 = {
    "8259", i8259_init, i8259_mask, i8259_unmask, i8259_masks, i8259_isr, i8259_begin, i8259_eoi,
};

static const struct irq_chip *chip;
static struct irq_stats stats;
static volatile uint64_t ticks;
static int handling;

void irq_init(void)
{
    if (read_rflags() & RFLAGS_IF)
        panic("irq: interrupts enabled before the controller was set up");
    chip = &i8259;
    chip->init(IRQ_VECTOR_BASE);
    pit_init();
    chip->unmask(IRQ_TIMER);

    uint16_t m = chip->masks();
    if (m != (uint16_t)~(1u << IRQ_TIMER))
        panic("irq: controller masks differ from IRQ0 only");
    serial_puts("irq: 8259 master 0x20-0x27, slave 0x28-0x2f (cascade IRQ2); masks ");
    serial_hex(m);
    serial_puts(" (IRQ0 only)\r\n");
    serial_puts("pit: channel 0 mode 2, divisor ");
    serial_dec(PIT_DIVISOR);
    uint64_t mhz = (uint64_t)PIT_INPUT_HZ * 1000 / PIT_DIVISOR;     /* 99998 */
    serial_puts(" (");
    serial_dec(mhz / 1000);
    serial_puts(".");
    serial_putc((char)('0' + mhz / 100 % 10));
    serial_putc((char)('0' + mhz / 10 % 10));
    serial_putc((char)('0' + mhz % 10));
    serial_puts(" Hz)\r\n");
}

void irq_handle(struct exc_frame *f)
{
    if (!chip || !irq_is_vector(f->vector))
        panic("irq: not a hardware interrupt vector");
    if (read_rflags() & RFLAGS_IF)
        panic("irq: handler running with interrupts enabled");
    if (handling)
        panic("irq: nested hardware interrupt");
    handling = 1;

    unsigned irq = (unsigned)(f->vector - IRQ_VECTOR_BASE);
    int r = chip->begin(irq);

    if (r == IRQ_NOT_IN_SERVICE)
        panic("irq: vector delivered but not in service at the controller");
    if (r == IRQ_SPURIOUS) {
        stats.spurious[irq]++;
    } else {
        stats.delivered[irq]++;
        if (irq == IRQ_TIMER)
            ticks++;
        else
            stats.unexpected++;         /* a masked line: counted, acknowledged, never handled */
        chip->eoi(irq);
        /* Nothing nests (IF = 0), so the acknowledged line must have left service. */
        if (chip->in_service() & (1u << irq))
            panic("irq: still in service at the controller after EOI");
    }
    handling = 0;
}

int irq_in_handler(void)
{
    return handling;
}

uint64_t irq_ticks(void)
{
    return ticks;
}

const struct irq_stats *irq_stats(void)
{
    return &stats;
}

const struct irq_chip *irq_chip(void)
{
    return chip;
}
