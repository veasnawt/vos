#include "idt.h"
#include "console.h"
#include "gdt.h"
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
extern char exc_stub_128[];

/*
 * All 256 vectors. Exception gates have DPL 0, so `int n` from ring 3 for any
 * of them raises #GP(n * 8 + 2); vectors 32-255 other than 0x80 are absent
 * with DPL 0, which also gives #GP for `int n` from ring 3.
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
    serial_puts("\r\nidt: vector 0x80 present, DPL 3 (provisional exit only)\r\n");
}

unsigned idt_gate_ist(unsigned vector)
{
    return vector < IDT_ENTRIES ? idt[vector].ist : 0xFF;
}

#ifdef VOS_TEST
void idt_clear_gate(unsigned vector)
{
    if (vector >= IDT_ENTRIES)
        panic("idt_clear_gate: bad vector");
    idt[vector].type_attr = 0;
}
#endif
