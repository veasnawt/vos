#include "gdt.h"
#include "console.h"
#include "cpu.h"
#include "panic.h"

#define KERNEL_ENTRY_STACK_SIZE 16384
#define IST_STACK_SIZE          4096
#define GUARD_SIZE              4096
#define STACK_CANARY            0xC0DEC0DEC0DEC0DEULL
#define CANARY_WORDS            8       /* at the lowest addresses: overflow hits them first */

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
/*
 * Each stack sits directly above its own page-aligned guard page, which the
 * W^X kernel map (kmap.c) leaves unmapped: an overflow faults instead of
 * running into other data. Canaries at the bottom of each stack stay.
 */
static struct { uint8_t guard[GUARD_SIZE]; uint8_t stack[KERNEL_ENTRY_STACK_SIZE]; }
    entry_stack __attribute__((aligned(4096)));
struct ist_stack { uint8_t guard[GUARD_SIZE]; uint8_t stack[IST_STACK_SIZE]; };
static struct ist_stack ist_stacks[3] __attribute__((aligned(4096)));  /* IST1 #DF, IST2 NMI, IST3 #MC */

#define kernel_entry_stack  entry_stack.stack
#define ist1_stack          ist_stacks[IST_DOUBLE_FAULT - 1].stack

static uint64_t *canary_base(unsigned i)   /* 0 = entry, 1..3 = IST1..IST3 */
{
    return (uint64_t *)(i == 0 ? entry_stack.stack : ist_stacks[i - 1].stack);
}

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

uint64_t gdt_tss_ist(unsigned n)
{
    return n >= 1 && n <= 7 ? tss.ist[n - 1] : 0;
}

unsigned gdt_guard_pages(uint64_t *va, unsigned max)
{
    unsigned n = 0;

    if (n < max)
        va[n++] = (uint64_t)entry_stack.guard;
    for (unsigned i = 0; i < 3 && n < max; i++)
        va[n++] = (uint64_t)ist_stacks[i].guard;
    return n;
}

const char *gdt_stack_name(uint64_t addr)
{
    static const char *const names[3] = { "ist1", "ist2", "ist3" };

    for (unsigned i = 0; i < 3; i++)
        if (addr >= (uint64_t)ist_stacks[i].stack && addr < (uint64_t)ist_stacks[i].stack + IST_STACK_SIZE)
            return names[i];
    if (addr >= (uint64_t)entry_stack.stack && addr < (uint64_t)entry_stack.stack + KERNEL_ENTRY_STACK_SIZE)
        return "entry";
    return 0;
}

int gdt_stack_canaries_intact(void)
{
    for (unsigned s = 0; s < 4; s++)
        for (int i = 0; i < CANARY_WORDS; i++)
            if (canary_base(s)[i] != STACK_CANARY)
                return 0;
    return 1;
}

#ifdef VOS_TEST
void gdt_test_smash_canary(int smash)
{
    ((uint64_t *)kernel_entry_stack)[CANARY_WORDS - 1] = smash ? 0 : STACK_CANARY;
}
#endif

int gdt_in_ist1_stack(uint64_t addr)
{
    return addr >= (uint64_t)ist1_stack && addr < (uint64_t)ist1_stack + IST_STACK_SIZE;
}

void gdt_init(void)
{
    for (unsigned s = 0; s < 4; s++)
        for (int i = 0; i < CANARY_WORDS; i++)
            canary_base(s)[i] = STACK_CANARY;
    tss.rsp[0] = (uint64_t)kernel_entry_stack + KERNEL_ENTRY_STACK_SIZE;
    for (unsigned i = 0; i < 3; i++)
        tss.ist[i] = (uint64_t)ist_stacks[i].stack + IST_STACK_SIZE;
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
    serial_puts(" ist2=");
    serial_hex(tss.ist[1]);
    serial_puts(" ist3=");
    serial_hex(tss.ist[2]);
    serial_puts(" iomap=");
    serial_dec(tss.iomap_base);
    serial_puts(" limit=");
    serial_dec(limit);
    serial_puts("\r\n");
}
