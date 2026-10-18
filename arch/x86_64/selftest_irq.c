/*
 * Hardware interrupts and timer (N2.1), VOS_TEST only.
 *
 * Two kinds of evidence, kept apart:
 *   synthetic  the 8259 driver (pic.c) run against a fake chip that records
 *              every port write and models IMR/ISR/IRR: init sequence,
 *              masks, EOI routing, spurious IRQ7/IRQ15, rejection of lines
 *              that are not in service;
 *   hardware   the real (QEMU-emulated) chips and PIT read back through their
 *              ports, the IDT gates, and interrupts actually delivered by the
 *              CPU while ring-3 programs run (proc_run with IF = 1).
 */
#include "selftest.h"
#include "console.h"
#include "cpu.h"
#include "exception.h"
#include "gdt.h"
#include "idt.h"
#include "irq.h"
#include "kmap.h"
#include "lib.h"
#include "pic.h"
#include "pit.h"

static int failures, cases;

static void expect(int ok, const char *what)
{
    cases++;
    if (ok)
        return;
    failures++;
    serial_puts("selftest: FAIL: ");
    serial_puts(what);
    serial_puts("\r\n");
}

/* ---- a fake 8259 pair ---- */

static struct {
    uint8_t imr[2], isr[2], irr[2], base[2], icw3[2], icw4[2];
    int step[2], read_isr[2];
    unsigned eois[2], writes;
    uint16_t port[32];
    uint8_t val[32];
} fk;

static int chip_of(uint16_t port)
{
    return port >= PIC_SLAVE_CMD;
}

static uint8_t fake_in(uint16_t port)
{
    int c = chip_of(port);

    if (port == PIC_MASTER_DATA || port == PIC_SLAVE_DATA)
        return fk.imr[c];
    return fk.read_isr[c] ? fk.isr[c] : fk.irr[c];
}

static void fake_out(uint16_t port, uint8_t v)
{
    int c = chip_of(port);

    if (fk.writes < 32) {
        fk.port[fk.writes] = port;
        fk.val[fk.writes] = v;
    }
    fk.writes++;
    if (port == PIC_MASTER_CMD || port == PIC_SLAVE_CMD) {
        if (v & 0x10) {                         /* ICW1 */
            fk.step[c] = 1;
            fk.imr[c] = fk.isr[c] = 0;
        } else if ((v & 0x18) == 0x08) {        /* OCW3 */
            fk.read_isr[c] = v & 1;
        } else if (v == PIC_OCW2_EOI) {         /* non-specific EOI: highest-priority bit */
            fk.eois[c]++;
            for (int b = 0; b < 8; b++)
                if (fk.isr[c] & (1u << b)) {
                    fk.isr[c] &= (uint8_t)~(1u << b);
                    break;
                }
        }
        return;
    }
    switch (fk.step[c]) {
    case 1: fk.base[c] = v; fk.step[c] = 2; break;
    case 2: fk.icw3[c] = v; fk.step[c] = 3; break;
    case 3: fk.icw4[c] = v; fk.step[c] = 0; break;
    default: fk.imr[c] = v; break;
    }
}

static const struct pic_io fake = { fake_in, fake_out };

static void fake_reset(void)
{
    memset(&fk, 0, sizeof(fk));
    pic_init(&fake, IRQ_VECTOR_BASE);
    fk.writes = 0;
    fk.eois[0] = fk.eois[1] = 0;
}

static void test_synthetic(void)
{
    static const uint16_t port[12] = { 0x21, 0xA1, 0x20, 0xA0, 0x21, 0xA1, 0x21, 0xA1, 0x21, 0xA1, 0x21, 0xA1 };
    static const uint8_t val[12] = { 0xFF, 0xFF, 0x11, 0x11, 0x20, 0x28, 0x04, 0x02, 0x01, 0x01, 0xFF, 0xFF };
    int seq = 1;

    memset(&fk, 0, sizeof(fk));
    fk.imr[0] = 0xB8;                   /* whatever the firmware left */
    pic_init(&fake, IRQ_VECTOR_BASE);
    for (int i = 0; i < 12; i++)
        seq &= fk.port[i] == port[i] && fk.val[i] == val[i];
    expect(fk.writes == 12 && seq, "synthetic: init masks first, then ICW1-4 to both chips, then masks all");
    expect(fk.base[0] == 0x20 && fk.base[1] == 0x28 && fk.icw3[0] == 0x04 && fk.icw3[1] == 0x02 &&
           fk.icw4[0] == 0x01 && fk.icw4[1] == 0x01 && pic_masks(&fake) == 0xFFFF,
           "synthetic: master 0x20, slave 0x28, cascade on IRQ2, 8086 mode, every line masked");

    fake_reset();
    pic_set_mask(&fake, 0, 0);
    expect(pic_masks(&fake) == 0xFFFE, "synthetic: unmask IRQ0 touches only master bit 0");
    pic_set_mask(&fake, 9, 0);
    expect(pic_masks(&fake) == 0xFDFE, "synthetic: unmask IRQ9 touches only slave bit 1");
    pic_set_mask(&fake, 9, 1);
    pic_set_mask(&fake, 0, 1);
    expect(pic_masks(&fake) == 0xFFFF, "synthetic: masking restores both IMRs");

    fake_reset();
    fk.isr[0] = 0x01;
    expect(pic_begin(&fake, 0) == PIC_GENUINE, "synthetic: IRQ0 in service is genuine");
    pic_eoi(&fake, 0);
    expect(fk.eois[0] == 1 && fk.eois[1] == 0 && fk.isr[0] == 0, "synthetic: IRQ0 EOI goes to the master only");

    fake_reset();
    expect(pic_begin(&fake, 0) == PIC_NOT_IN_SERVICE && fk.eois[0] == 0,
           "synthetic: IRQ0 without in-service bit is refused, not acknowledged");

    fake_reset();
    expect(pic_begin(&fake, 7) == PIC_SPURIOUS && fk.eois[0] == 0 && fk.eois[1] == 0,
           "synthetic: spurious IRQ7 (no in-service bit) gets no EOI");
    fake_reset();
    fk.isr[0] = 0x80;
    expect(pic_begin(&fake, 7) == PIC_GENUINE, "synthetic: IRQ7 with its in-service bit is genuine");

    fake_reset();
    fk.isr[0] = 0x04;                   /* the master saw the cascade line */
    expect(pic_begin(&fake, 15) == PIC_SPURIOUS && fk.eois[0] == 1 && fk.eois[1] == 0,
           "synthetic: spurious IRQ15 gets an EOI on the master only");

    fake_reset();
    fk.isr[0] = 0x04;
    fk.isr[1] = 0x10;
    expect(pic_begin(&fake, 12) == PIC_GENUINE, "synthetic: IRQ12 in service on slave and cascade is genuine");
    pic_eoi(&fake, 12);
    expect(fk.eois[0] == 1 && fk.eois[1] == 1 && fk.isr[0] == 0 && fk.isr[1] == 0,
           "synthetic: slave IRQ EOI goes to the slave and the master");

    fake_reset();
    fk.isr[1] = 0x10;
    expect(pic_begin(&fake, 12) == PIC_NOT_IN_SERVICE, "synthetic: slave line without the cascade bit is refused");
}

/* ---- the real chips, the PIT and the IDT ---- */

extern const uint64_t irq_stub_table[IRQ_LINES];

static void test_hardware_state(void)
{
    const struct irq_chip *c = irq_chip();
    uint8_t status;
    uint16_t count, prev;
    unsigned gates = 1;

    expect(c && (irq_expected_masks() & 1) == 0 && c->masks() == irq_expected_masks(),
           "8259 IMRs read back: IRQ0 and the registered device lines unmasked, nothing else");
    expect(c && c->in_service() == 0, "8259 nothing in service while the kernel runs");
    pit_readback(&status, &count);
    expect((status & PIT_STATUS_CFG_MASK) == 0x34, "PIT read-back status: channel 0, lobyte/hibyte, mode 2, binary");
    expect(count > 0 && count <= PIT_DIVISOR, "PIT channel 0 count within the divisor");
    prev = count;
    for (int i = 0; i < 1000 && count == prev; i++)
        pit_readback(&status, &count);
    expect(count != prev, "PIT channel 0 is counting");

    for (unsigned v = 0; v < IDT_ENTRIES; v++) {
        unsigned t = idt_gate_type(v);

        if (v < IDT_VECTORS)
            gates &= t == 0x8E;
        else if (irq_is_vector(v))
            gates &= t == 0x8E && idt_gate_ist(v) == 0 &&
                     idt_gate_handler(v) == irq_stub_table[v - IRQ_VECTOR_BASE];
        else if (v == IDT_SYSCALL)
            gates &= t == 0xEE && idt_gate_ist(v) == 0;
        else
            gates &= t == 0;
    }
    expect(gates, "IDT: 0x00-0x2f DPL 0 interrupt gates, IRQs on their own stubs without IST, 0x80 DPL 3, rest absent");
    expect(!(read_rflags() & RFLAGS_IF), "kernel self-test runs with IF=0");

    /* The N2.0 kmap probes: armed only inside kmap_init, which cannot run again. */
    struct exc_frame f;
    memset(&f, 0, sizeof(f));
    f.vector = 14;
    f.cs = GDT_KERNEL_CS;
    f.rip = kmap_test_probe_insn();
    expect(kmap_init(0) != 0 && kmap_probe_fault(&f) == 0 && f.rip == kmap_test_probe_insn(),
           "kmap probes inert after boot: kmap_init refuses to rerun, a #PF at the probe is not recovered");
}

/* ---- N3.4: device-line handlers, on lines nothing uses (IF stays 0: nothing is delivered) ---- */

static unsigned top_calls[3], bottom_calls[3], call_seq[3], seq;
static int top_says[3];

static int test_top(void *ctx)
{
    unsigned i = (unsigned)(uintptr_t)ctx;

    top_calls[i]++;
    call_seq[i] = ++seq;
    return top_says[i];
}

static void test_bottom(void *ctx)
{
    bottom_calls[(unsigned)(uintptr_t)ctx]++;
}

static void reset_calls(void)
{
    memset(top_calls, 0, sizeof(top_calls));
    memset(bottom_calls, 0, sizeof(bottom_calls));
    memset(call_seq, 0, sizeof(call_seq));
    seq = 0;
}

/* A line other than 0, 2 and `except`, masked now (no handler), with the wanted trigger mode, preferring the slave. */
static int free_line(int level, int except)
{
    uint16_t trig = irq_chip()->triggers(), masks = irq_expected_masks();

    for (int l = IRQ_LINES - 1; l > 0; l--)
        if (l != PIC_CASCADE_IRQ && l != except && (masks & (1u << l)) && !!(trig & (1u << l)) == level &&
            !irq_line_disabled(l))
            return l;
    return -1;
}

static int not_applicable;

/* U1: a check this machine cannot exercise (no level-triggered line): reported, never counted as passed. */
static void not_applicable_here(const char *what, const char *reason)
{
    not_applicable++;
    serial_puts("selftest: NOT APPLICABLE: ");
    serial_puts(what);
    serial_puts(" (");
    serial_puts(reason);
    serial_puts(")\r\n");
}

static void test_handlers(void)
{
    const struct irq_stats *st = irq_stats();
    const struct irq_chip *c = irq_chip();
    uint16_t trig = c->triggers(), m0 = c->masks();
    int edge = free_line(0, -1), level = free_line(1, -1), h[IRQ_MAX_HANDLERS + 1], hh, n;
    /*
     * U1: OVMF leaves every line edge triggered (ELCR 0). The tests that need
     * a second line then use another free edge line; only the checks that
     * need a level-triggered line are reported as not applicable.
     */
    int second = level > 0 ? level : free_line(0, edge);
    int second_trig = level > 0 ? IRQ_TRIGGER_LEVEL : IRQ_TRIGGER_EDGE;

    serial_puts("selftest: irq: ELCR ");
    serial_hex(trig);
    serial_puts(", test lines: edge ");
    serial_dec((uint64_t)edge);
    serial_puts(", level ");
    serial_dec((uint64_t)level);
    serial_puts("\r\n");
    expect(!(trig & ((1u << IRQ_TIMER) | (1u << 1) | (1u << PIC_CASCADE_IRQ))),
           "ELCR: the timer, keyboard and cascade lines are edge triggered");
    expect(edge >= 8 && second > 0, "a free slave edge line and a second free line exist for the tests");
    if (edge < 8 || second <= 0)
        return;
    if (level <= 0)
        not_applicable_here("level-triggered registration and its trigger-mode refusal",
                            "no line is level triggered in the ELCR");

    expect(irq_register(IRQ_TIMER, IRQ_TRIGGER_EDGE, "t", test_top, 0, 0, &hh) == IRQ_EINVAL &&
           irq_register(PIC_CASCADE_IRQ, IRQ_TRIGGER_EDGE, "t", test_top, 0, 0, &hh) == IRQ_EINVAL &&
           irq_register(IRQ_LINES, IRQ_TRIGGER_EDGE, "t", test_top, 0, 0, &hh) == IRQ_EINVAL &&
           irq_register((unsigned)edge, IRQ_TRIGGER_EDGE, "t", 0, 0, 0, &hh) == IRQ_EINVAL &&
           irq_register((unsigned)edge, 7, "t", test_top, 0, 0, &hh) == IRQ_EINVAL,
           "registration refuses the timer, the cascade, line 16, a missing top half and a bad trigger mode");
    expect(irq_register((unsigned)edge, IRQ_TRIGGER_LEVEL, "t", test_top, 0, 0, &hh) == IRQ_ETRIGGER &&
           c->masks() == m0, "registration refuses a level handler on an edge-triggered line; masks unchanged");
    if (level > 0)
        expect(irq_register((unsigned)level, IRQ_TRIGGER_EDGE, "t", test_top, 0, 0, &hh) == IRQ_ETRIGGER &&
               c->masks() == m0, "registration refuses an edge handler on a level-triggered line; masks unchanged");

    /* Two handlers sharing a slave line. */
    reset_calls();
    int ok = irq_register((unsigned)edge, IRQ_TRIGGER_EDGE, "t0", test_top, test_bottom, (void *)0, &h[0]) == IRQ_OK &&
             irq_register((unsigned)edge, IRQ_TRIGGER_EDGE, "t1", test_top, test_bottom, (void *)1, &h[1]) == IRQ_OK;
    uint16_t m1 = c->masks();
    expect(ok && h[0] != h[1] && !(m1 & (1u << edge)) && !(m1 & (1u << PIC_CASCADE_IRQ)) &&
           (m1 | (1u << edge) | (1u << PIC_CASCADE_IRQ)) == (m0 | (1u << PIC_CASCADE_IRQ)) &&
           m1 == irq_expected_masks() && irq_audit() == NULL,
           "registering a slave line unmasks exactly it and the cascade; audit clean");

    uint64_t cl0 = st->claimed[edge], un0 = st->unclaimed[edge], br0 = st->bottom_runs;
    int other = irq_register((unsigned)second, second_trig, "t2", test_top, test_bottom, (void *)2, &hh) == IRQ_OK;
    top_says[0] = IRQ_NONE;
    top_says[1] = IRQ_HANDLED;
    top_says[2] = IRQ_HANDLED;
    n = (int)irq_test_dispatch((unsigned)edge);
    expect(n == 1 && top_calls[0] == 1 && top_calls[1] == 1 && call_seq[0] == 1 && call_seq[1] == 2 &&
           bottom_calls[0] == 0 && bottom_calls[1] == 0 && st->claimed[edge] == cl0 + 1,
           "shared line: every top half is asked, in registration order; nothing deferred runs inside the handler");
    irq_run_deferred();
    irq_run_deferred();
    expect(bottom_calls[0] == 0 && bottom_calls[1] == 1 && st->bottom_runs == br0 + 1,
           "only the claiming handler's bottom half runs, once, in thread context");
    expect(other && top_calls[2] == 0 && bottom_calls[2] == 0 && irq_unregister(hh) == IRQ_OK,
           "a handler of another line is never asked");
    top_says[2] = IRQ_NONE;
    top_says[1] = IRQ_NONE;
    n = (int)irq_test_dispatch((unsigned)edge);
    irq_run_deferred();
    expect(n == 0 && st->unclaimed[edge] == un0 + 1 && bottom_calls[0] == 0 && bottom_calls[1] == 1,
           "an interrupt no handler claims is counted unclaimed and runs no bottom half");

    /* A storm: more than IRQ_STORM_LIMIT interrupts within one tick (no tick can pass: IF = 0). */
    uint64_t s0 = st->storms, t0 = irq_ticks();
    for (unsigned i = 0; i <= IRQ_STORM_LIMIT; i++)
        irq_test_dispatch((unsigned)edge);
    uint16_t m2 = c->masks();
    expect(irq_ticks() == t0 && st->storms == s0 + 1 && irq_line_disabled((unsigned)edge) && (m2 & (1u << edge)) &&
           m2 == irq_expected_masks() && irq_audit() == NULL,
           "a storm (1001 interrupts in one tick) masks the line for good; masks match the table");
    irq_run_deferred();
    expect(bottom_calls[0] == 1 && bottom_calls[1] == 2 &&
           irq_register((unsigned)edge, IRQ_TRIGGER_EDGE, "t2", test_top, 0, 0, &hh) == IRQ_EDISABLED,
           "after a storm every handler of the line runs its bottom half once; new registrations are refused");

    /* An unclaimed storm spread over two ticks: 1001 in a row that nobody claims also masks the line. */
    irq_test_clear_storm((unsigned)edge);
    s0 = st->storms;
    for (unsigned i = 0; i < IRQ_STORM_LIMIT / 2; i++)
        irq_test_dispatch((unsigned)edge);
    uint64_t t1 = irq_ticks();
    thread_sleep(1);                    /* a tick passes: the per-tick count starts again */
    for (unsigned i = IRQ_STORM_LIMIT / 2; i <= IRQ_STORM_LIMIT; i++)
        irq_test_dispatch((unsigned)edge);
    irq_run_deferred();
    expect(irq_ticks() > t1 && st->storms == s0 + 1 && irq_line_disabled((unsigned)edge),
           "1001 unclaimed interrupts in a row, across a tick, also mask the line");
    t0 = irq_ticks();

    /* A claimed storm: the device acknowledges, yet interrupts 1001 times within one tick. */
    irq_test_clear_storm((unsigned)edge);
    top_says[1] = IRQ_HANDLED;
    s0 = st->storms;
    for (unsigned i = 0; i <= IRQ_STORM_LIMIT; i++)
        irq_test_dispatch((unsigned)edge);
    irq_run_deferred();
    expect(irq_ticks() == t0 && st->storms == s0 + 1 && irq_line_disabled((unsigned)edge) && irq_audit() == NULL,
           "a storm of claimed interrupts (1001 in one tick) also masks the line");
    top_says[1] = IRQ_NONE;

    expect(irq_unregister(h[0]) == IRQ_OK && irq_unregister(h[1]) == IRQ_OK && irq_unregister(h[1]) == IRQ_EINVAL &&
           irq_unregister(0) == IRQ_EINVAL && irq_unregister(IRQ_MAX_HANDLERS + 1) == IRQ_EINVAL,
           "unregistration; a stale, zero or out-of-range handle is refused");
    irq_test_clear_storm((unsigned)edge);
    expect(c->masks() == m0 && irq_audit() == NULL, "masks back to their state before the tests");

    /* The table is bounded; a level line (where one exists) takes a level handler. */
    unsigned made = 0;
    int full = 0;
    for (unsigned i = 0; i <= IRQ_MAX_HANDLERS; i++) {
        int r = irq_register((unsigned)second, second_trig, "t", test_top, 0, (void *)2, &h[made]);

        if (r == IRQ_OK)
            made++;
        else
            full = r == IRQ_EFULL;
    }
    uint16_t m3 = c->masks();
    for (unsigned i = 0; i < made; i++)
        irq_unregister(h[i]);
    expect(made >= 1 && full && !(m3 & (1u << second)) && c->masks() == m0 && irq_audit() == NULL,
           level > 0 ? "a level line accepts a level handler; the table refuses one beyond IRQ_MAX_HANDLERS"
                     : "a second edge line accepts handlers; the table refuses one beyond IRQ_MAX_HANDLERS");
}

int selftest_irq(void)
{
    failures = cases = not_applicable = 0;
    test_synthetic();
    test_hardware_state();
    test_handlers();
    serial_puts("selftest: irq ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed, ");
    serial_dec(not_applicable);
    serial_puts(" not applicable\r\n");
    return failures;
}
