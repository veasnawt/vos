#include "console.h"
#include "cpu.h"
#include "memlayout.h"

#define COM1            0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* disable interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x03);   /* 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);   /* FIFO */
    outb(COM1 + 4, 0x03);
}

void serial_puts(const char *s)
{
    for (; *s; s++) {
        while (!(inb(COM1 + 5) & 0x20))
            ;
        outb(COM1, (uint8_t)*s);
    }
}

void serial_hex(uint64_t v)
{
    char buf[19];
    int i = sizeof(buf) - 1;

    buf[i] = '\0';
    do {
        buf[--i] = "0123456789abcdef"[v & 0xF];
        v >>= 4;
    } while (v);
    buf[--i] = 'x';
    buf[--i] = '0';
    serial_puts(&buf[i]);
}

void serial_dec(uint64_t v)
{
    char buf[21];
    int i = sizeof(buf) - 1;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    serial_puts(&buf[i]);
}

void vga_puts(const char *s)
{
    volatile uint16_t *vga = phys_to_virt(VGA_TEXT_PHYS);

    for (int i = 0; i < 80 * 25; i++)
        vga[i] = 0x0720;
    for (int i = 0; s[i] && i < 80 * 25; i++)
        vga[i] = 0x0700 | (uint8_t)s[i];
}
