#include "gdt.h"
#include "console.h"
#include "cpu.h"
#include "panic.h"

#define KERNEL_ENTRY_STACK_SIZE 16384
#define IST1_STACK_SIZE         4096

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];                    /* RSP0-RSP2 */
    uint64_t reserved1;
    uint64_t ist[7];                    /* IST1-IST7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* The 16-byte TSS descriptor occupies entries 5 and 6. */
static uint64_t gdt[7] __attribute__((aligned(16))) = {
    0,
    0x00AF9A000000FFFFULL,              /* 0x08 kernel code: 64-bit, DPL 0 */
    0x00CF92000000FFFFULL,              /* 0x10 kernel data: DPL 0 */
    0x00CFF2000000FFFFULL,              /* 0x18 user data: DPL 3 */
    0x00AFFA000000FFFFULL,              /* 0x20 user code: 64-bit, DPL 3 */
};

static struct tss tss __attribute__((aligned(16)));
static uint8_t kernel_entry_stack[KERNEL_ENTRY_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t ist1_stack[IST1_STACK_SIZE] __attribute__((aligned(16)));

uint64_t gdt_tss_base(void)
{
    return (uint64_t)&tss;
}

uint64_t gdt_tss_rsp0(void)
{
    return tss.rsp[0];
}

uint64_t gdt_tss_ist1(void)
{
    return tss.ist[IST_DOUBLE_FAULT - 1];
}

int gdt_in_ist1_stack(uint64_t addr)
{
    return addr >= (uint64_t)ist1_stack && addr < (uint64_t)ist1_stack + sizeof(ist1_stack);
}

void gdt_init(void)
{
    tss.rsp[0] = (uint64_t)kernel_entry_stack + sizeof(kernel_entry_stack);
    tss.ist[IST_DOUBLE_FAULT - 1] = (uint64_t)ist1_stack + sizeof(ist1_stack);
    /* Base beyond the TSS limit: no I/O permission bitmap, so CPL 3 gets no port access. */
    tss.iomap_base = sizeof(tss);

    uint64_t base = (uint64_t)&tss, limit = sizeof(tss) - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    struct gdt_ptr gp = { sizeof(gdt) - 1, (uint64_t)gdt };
    __asm__ volatile (
        "lgdt %0\n\t"
        "pushq %1\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "mov %2, %%ds\n\t"
        "mov %2, %%es\n\t"
        "mov %2, %%ss\n\t"
        "xor %%eax, %%eax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs"
        : : "m"(gp), "i"((uint64_t)GDT_KERNEL_CS), "r"((uint16_t)GDT_KERNEL_DS) : "rax", "memory");
    __asm__ volatile ("ltr %w0" : : "r"((uint16_t)GDT_TSS));

    uint16_t tr, cs, ss;
    __asm__ volatile ("str %0" : "=r"(tr));
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile ("mov %%ss, %0" : "=r"(ss));
    /* ltr marks the descriptor busy (type 0xB). */
    int busy = ((gdt[5] >> 40) & 0xF) == 0xB;
    if (tr != GDT_TSS || cs != GDT_KERNEL_CS || ss != GDT_KERNEL_DS || !busy)
        panic("GDT/TSS load verification failed");

    serial_puts("gdt: loaded cs=");
    serial_hex(cs);
    serial_puts(" ss=");
    serial_hex(ss);
    serial_puts(" tr=");
    serial_hex(tr);
    serial_puts(" tss=busy\r\n");
    serial_puts("tss: rsp0=");
    serial_hex(tss.rsp[0]);
    serial_puts(" ist1=");
    serial_hex(tss.ist[0]);
    serial_puts(" iomap=");
    serial_dec(tss.iomap_base);
    serial_puts(" limit=");
    serial_dec(limit);
    serial_puts("\r\n");
}
