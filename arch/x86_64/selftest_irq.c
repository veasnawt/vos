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

    expect(c && c->masks() == 0xFFFE, "8259 IMRs read back: only IRQ0 unmasked (master 0xfe, slave 0xff)");
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

int selftest_irq(void)
{
    failures = cases = 0;
    test_synthetic();
    test_hardware_state();
    serial_puts("selftest: irq ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}
