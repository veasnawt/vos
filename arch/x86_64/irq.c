#include "irq.h"
#include "console.h"
#include "cpu.h"
#include "exception.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"
#include "lib.h"

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

static uint16_t i8259_triggers(void)
{
    return pic_elcr(&pic_port_io);
}

static const struct irq_chip i8259 = {
    "8259", i8259_init, i8259_mask, i8259_unmask, i8259_masks, i8259_isr, i8259_begin, i8259_eoi, i8259_triggers,
};

static const struct irq_chip *chip;
static struct irq_stats stats;
static volatile uint64_t ticks;
static int handling;

/* N3.4: device-line handlers. */
struct irq_handler {
    int used;
    unsigned line;
    const char *name;
    irq_top_fn top;
    irq_bottom_fn bottom;
    void *ctx;
    int pending;                        /* the top half claimed an interrupt; the bottom half has not run */
};
static struct irq_handler handlers[IRQ_MAX_HANDLERS];
static uint16_t disabled;               /* lines masked after a storm */
static uint64_t burst_tick[IRQ_LINES];
static unsigned burst[IRQ_LINES], unclaimed_run[IRQ_LINES];

uint16_t irq_expected_masks(void)
{
    uint16_t on = 1u << IRQ_TIMER;

    for (unsigned i = 0; i < IRQ_MAX_HANDLERS; i++)
        if (handlers[i].used && !(disabled & (1u << handlers[i].line)))
            on |= (uint16_t)(1u << handlers[i].line);
    if (on & 0xFF00)
        on |= 1u << PIC_CASCADE_IRQ;
    return (uint16_t)~on;
}

/* Bring the controller's masks to irq_expected_masks, line by line. */
static void apply_masks(void)
{
    uint16_t want = irq_expected_masks(), have = chip->masks();

    for (unsigned l = 0; l < IRQ_LINES; l++) {
        uint16_t b = (uint16_t)(1u << l);

        if ((want & b) && !(have & b))
            chip->mask(l);
        else if (!(want & b) && (have & b))
            chip->unmask(l);
    }
}

static void scheduler_context(const char *what)
{
    if (read_rflags() & RFLAGS_IF)
        panic(what);
    if (handling)
        panic(what);
}

int irq_register(unsigned line, int trigger, const char *name, irq_top_fn top, irq_bottom_fn bottom, void *ctx,
                 int *handle)
{
    scheduler_context("irq: register outside thread context");
    if (!chip || line >= IRQ_LINES || line == IRQ_TIMER || line == PIC_CASCADE_IRQ || !top || !handle ||
        (trigger != IRQ_TRIGGER_EDGE && trigger != IRQ_TRIGGER_LEVEL))
        return IRQ_EINVAL;
    if (!!(chip->triggers() & (1u << line)) != (trigger == IRQ_TRIGGER_LEVEL))
        return IRQ_ETRIGGER;
    if (disabled & (1u << line))
        return IRQ_EDISABLED;
    for (unsigned i = 0; i < IRQ_MAX_HANDLERS; i++) {
        struct irq_handler *h = &handlers[i];

        if (h->used)
            continue;
        h->used = 1;
        h->line = line;
        h->name = name;
        h->top = top;
        h->bottom = bottom;
        h->ctx = ctx;
        h->pending = 0;
        apply_masks();
        *handle = (int)i + 1;
        return IRQ_OK;
    }
    return IRQ_EFULL;
}

int irq_unregister(int handle)
{
    scheduler_context("irq: unregister outside thread context");
    if (handle < 1 || handle > IRQ_MAX_HANDLERS || !handlers[handle - 1].used)
        return IRQ_EINVAL;
    memset(&handlers[handle - 1], 0, sizeof(handlers[0]));
    apply_masks();
    return IRQ_OK;
}

void irq_run_deferred(void)
{
    scheduler_context("irq: bottom halves outside thread context");
    for (unsigned i = 0; i < IRQ_MAX_HANDLERS; i++) {
        struct irq_handler *h = &handlers[i];

        if (!h->used || !h->pending)
            continue;
        h->pending = 0;                 /* first: a bottom half that waits for the next interrupt re-arms it */
        stats.bottom_runs++;
        if (h->bottom)
            h->bottom(h->ctx);
    }
}

int irq_line_disabled(unsigned line)
{
    return line < IRQ_LINES && (disabled & (1u << line));
}

/*
 * The top halves of device line irq (inside irq_handle, before the EOI).
 * Returns how many claimed it. A storm masks the line, and its handlers'
 * bottom halves are scheduled so their drivers notice: more than
 * IRQ_STORM_LIMIT interrupts within one tick, or more than IRQ_STORM_LIMIT
 * in a row that nobody claimed (a level line nobody acknowledges fires
 * again right after each EOI, however long each round takes).
 */
static unsigned dispatch(unsigned irq)
{
    unsigned n = 0, mine = 0;

    for (unsigned i = 0; i < IRQ_MAX_HANDLERS; i++) {
        struct irq_handler *h = &handlers[i];

        if (!h->used || h->line != irq)
            continue;
        n++;
        if (h->top(h->ctx) == IRQ_HANDLED) {
            h->pending = 1;
            mine++;
        }
    }
    if (!n)
        stats.unexpected++;             /* a masked line: counted, acknowledged, never handled */
    else if (mine)
        stats.claimed[irq]++;
    else
        stats.unclaimed[irq]++;
    unclaimed_run[irq] = mine ? 0 : unclaimed_run[irq] + 1;
    if (burst_tick[irq] != ticks) {
        burst_tick[irq] = ticks;
        burst[irq] = 0;
    }
    if ((++burst[irq] > IRQ_STORM_LIMIT || unclaimed_run[irq] > IRQ_STORM_LIMIT) && !(disabled & (1u << irq))) {
        disabled |= (uint16_t)(1u << irq);
        apply_masks();                  /* the line, and the cascade if no other slave line remains */
        stats.storms++;
        for (unsigned i = 0; i < IRQ_MAX_HANDLERS; i++)
            if (handlers[i].used && handlers[i].line == irq)
                handlers[i].pending = 1;
    }
    return mine;
}

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
    serial_puts(" (IRQ0 only); level-triggered lines (ELCR) ");
    serial_hex(chip->triggers());
    serial_puts("\r\n");
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
            dispatch(irq);              /* each device acknowledged itself: a level line has dropped */
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

const char *irq_audit(void)
{
    if (!chip)
        return "no interrupt controller";
    if (handling)
        return "audit inside an interrupt handler";
    for (unsigned i = 0; i < IRQ_MAX_HANDLERS; i++) {
        const struct irq_handler *h = &handlers[i];

        if (!h->used) {
            if (h->top || h->bottom || h->pending || h->ctx)
                return "a free handler record is not clean";
            continue;
        }
        if (h->line >= IRQ_LINES || h->line == IRQ_TIMER || h->line == PIC_CASCADE_IRQ || !h->top)
            return "a handler is registered on an invalid line";
    }
    if (disabled & ((1u << IRQ_TIMER) | (1u << PIC_CASCADE_IRQ)))
        return "the timer or cascade line is marked disabled";
    if (chip->masks() != irq_expected_masks())
        return "controller masks differ from the registered lines";
    if (chip->in_service())
        return "an interrupt is in service outside a handler";
    return NULL;
}

#ifdef VOS_TEST
unsigned irq_test_dispatch(unsigned line)
{
    unsigned r;

    if (line >= IRQ_LINES || line == IRQ_TIMER || line == PIC_CASCADE_IRQ || handling)
        return 0;
    handling = 1;
    r = dispatch(line);
    handling = 0;
    return r;
}

void irq_test_clear_storm(unsigned line)
{
    if (line < IRQ_LINES) {
        disabled &= (uint16_t)~(1u << line);
        burst[line] = unclaimed_run[line] = 0;
        apply_masks();
    }
}
#endif
