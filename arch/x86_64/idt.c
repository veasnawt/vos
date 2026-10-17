#include "idt.h"
#include "console.h"
#include "gdt.h"
#include "panic.h"

#define IDT_INTERRUPT_GATE_DPL0 0x8E    /* present, DPL 0, 64-bit interrupt gate */

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

static struct idt_entry idt[IDT_VECTORS] __attribute__((aligned(16)));

void idt_init(void)
{
    for (unsigned i = 0; i < IDT_VECTORS; i++) {
        uint64_t h = exc_stub_table[i];

        idt[i].offset_lo = h & 0xFFFF;
        idt[i].selector = GDT_KERNEL_CS;
        idt[i].ist = i == 8 ? IST_DOUBLE_FAULT : 0;
        idt[i].type_attr = IDT_INTERRUPT_GATE_DPL0;
        idt[i].offset_mid = (h >> 16) & 0xFFFF;
        idt[i].offset_hi = h >> 32;
    }

    struct idt_ptr ip = { sizeof(idt) - 1, (uint64_t)idt };
    __asm__ volatile ("lidt %0" : : "m"(ip));
    serial_puts("idt: ");
    serial_dec(IDT_VECTORS);
    serial_puts(" exception vectors loaded, df ist=");
    serial_dec(idt[8].ist);
    serial_puts("\r\n");
}

#ifdef VOS_TEST
void idt_clear_gate(unsigned vector)
{
    if (vector >= IDT_VECTORS)
        panic("idt_clear_gate: bad vector");
    idt[vector].type_attr = 0;
}
#endif
