#include <stdint.h>

#define COM1 0x3F8
#define VGA ((volatile uint16_t *)0xB8000)

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* disable interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x03);   /* 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);   /* FIFO */
    outb(COM1 + 4, 0x03);
}

static void serial_puts(const char *s)
{
    for (; *s; s++) {
        while (!(inb(COM1 + 5) & 0x20))
            ;
        outb(COM1, (uint8_t)*s);
    }
}

static void vga_puts(const char *s)
{
    for (int i = 0; i < 80 * 25; i++)
        VGA[i] = 0x0720;
    for (int i = 0; s[i]; i++)
        VGA[i] = 0x0700 | (uint8_t)s[i];
}

void kernel_main(uint32_t magic, uint32_t mbi)
{
    (void)mbi;
    serial_init();
    vga_puts("Hello from VOS!");
    serial_puts("Hello from VOS!\r\n");
    serial_puts(magic == 0x2BADB002 ? "Multiboot magic OK\r\n"
                                    : "Multiboot magic BAD\r\n");
}
