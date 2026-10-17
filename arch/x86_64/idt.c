#include "idt.h"
#include "console.h"
#include "gdt.h"
#include "irq.h"
#include "panic.h"

#define IDT_INTERRUPT_GATE_DPL0 0x8E    /* present, DPL 0, 64-bit interrupt gate */
#define IDT_INTERRUPT_GATE_DPL3 0xEE    /* present, DPL 3: reachable by int $n from ring 3 */

struct idt_entry {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern const uint64_t exc_stub_table[IDT_VECTORS];  /* entry.S */
extern const uint64_t irq_stub_table[IRQ_LINES];
extern char exc_stub_128[];

/*
 * All 256 vectors. Exception and IRQ gates have DPL 0, so `int n` from ring 3
 * for any of them raises #GP(n * 8 + 2); vectors 0x30-0xFF other than 0x80
 * are absent with DPL 0, which also gives #GP for `int n` from ring 3. The
 * IRQ gates (0x20-0x2F) exist before the interrupt controller is set up, so
 * no interrupt can ever reach an uninitialized entry.
 */
static struct idt_entry idt[IDT_ENTRIES] __attribute__((aligned(16)));

static void set_gate(unsigned v, uint64_t h, uint8_t ist, uint8_t type_attr)
{
    idt[v].offset_lo = h & 0xFFFF;
    idt[v].selector = GDT_KERNEL_CS;
    idt[v].ist = ist;
    idt[v].type_attr = type_attr;
    idt[v].offset_mid = (h >> 16) & 0xFFFF;
    idt[v].offset_hi = h >> 32;
}

void idt_init(void)
{
    for (unsigned i = 0; i < IDT_VECTORS; i++)
        set_gate(i, exc_stub_table[i],
                 i == 8 ? IST_DOUBLE_FAULT : i == 2 ? IST_NMI : i == 18 ? IST_MACHINE_CHECK : 0,
                 IDT_INTERRUPT_GATE_DPL0);
    for (unsigned i = 0; i < IRQ_LINES; i++)
        set_gate(IRQ_VECTOR_BASE + i, irq_stub_table[i], 0, IDT_INTERRUPT_GATE_DPL0);
    /* Interrupt gate (IF stays 0); the stub runs on TSS.RSP0 when entered from ring 3. */
    set_gate(IDT_SYSCALL, (uint64_t)exc_stub_128, 0, IDT_INTERRUPT_GATE_DPL3);

    struct idt_ptr ip = { sizeof(idt) - 1, (uint64_t)idt };
    __asm__ volatile ("lidt %0" : : "m"(ip));
    serial_puts("idt: ");
    serial_dec(IDT_VECTORS);
    serial_puts(" exception vectors loaded, df ist=");
    serial_dec(idt[8].ist);
    serial_puts(", nmi ist=");
    serial_dec(idt[2].ist);
    serial_puts(", mc ist=");
    serial_dec(idt[18].ist);
    serial_puts("\r\nidt: IRQ vectors 0x20-0x2f present, DPL 0, interrupt gates\r\n");
    serial_puts("idt: vector 0x80 present, DPL 3 (system calls, ABI v0)\r\n");
}

unsigned idt_gate_ist(unsigned vector)
{
    return vector < IDT_ENTRIES ? idt[vector].ist : 0xFF;
}

unsigned idt_gate_type(unsigned vector)
{
    return vector < IDT_ENTRIES ? idt[vector].type_attr : 0;
}

uint64_t idt_gate_handler(unsigned vector)
{
    if (vector >= IDT_ENTRIES)
        return 0;
    return idt[vector].offset_lo | (uint64_t)idt[vector].offset_mid << 16 |
           (uint64_t)idt[vector].offset_hi << 32;
}

#ifdef VOS_TEST
void idt_clear_gate(unsigned vector)
{
    if (vector >= IDT_ENTRIES)
        panic("idt_clear_gate: bad vector");
    idt[vector].type_attr = 0;
}
#endif
