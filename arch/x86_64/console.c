#include "console.h"
#include "cpu.h"
#include "screen.h"

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

static void serial_raw(char c)
{
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, (uint8_t)c);
}

void serial_putc(char c)
{
    serial_raw(c);
    screen_putc(c);                     /* U1: mirrored on the screen console, if any (screen.h) */
}

void serial_log_putc(char c)
{
    serial_raw(c);
}

void console_erase_previous(void)
{
    serial_raw('\b');
    serial_raw(' ');
    serial_raw('\b');
    screen_erase_previous();
}

void serial_puts(const char *s)
{
    for (; *s; s++)
        serial_putc(*s);
}

void serial_puts_safe(const char *s)
{
    char buf[2] = { 0, 0 };
    int n = 0;

    for (; *s; s++, n++) {
        if (n == 120) {
            serial_puts("...");
            return;
        }
        buf[0] = (*s >= 0x20 && *s < 0x7F) ? *s : '?';
        serial_puts(buf);
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
