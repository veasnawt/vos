/* Serial (COM1) and VGA text output. */
#ifndef VOS_X86_64_CONSOLE_H
#define VOS_X86_64_CONSOLE_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
/* Like serial_puts but prints only printable ASCII (others as '?'), at most 120 chars. */
void serial_puts_safe(const char *s);
void serial_hex(uint64_t v);
void serial_dec(uint64_t v);
/* Clears the screen, then writes s from the top-left corner. */
void vga_puts(const char *s);

#endif
