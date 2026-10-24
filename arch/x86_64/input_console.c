#include "input_console.h"
#include "console.h"
#include "cpu.h"
#include "keyboard.h"
#include "screen.h"
#include "thread.h"

int input_console_requested(const char *cmdline)
{
    static const char token[] = "vos.console=1";
    const char *p = cmdline;
    if (!p)
        return 0;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        unsigned i = 0;
        while (token[i] && p[i] && token[i] == p[i])
            i++;
        if (!token[i] && (!p[i] || p[i] == ' ' || p[i] == '\t'))
            return 1;
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    return 0;
}

static uint8_t port_in(uint16_t port) { return inb(port); }
static void port_out(uint16_t port, uint8_t value) { outb(port, value); }
static const struct keyboard_io port_io = { port_in, port_out };

void input_console_accept(struct input_console_line *line, char c)
{
    if (c == '\n') {
        serial_puts("\r\ninput> ");
        line->length = 0;
    } else if (c == '\b') {
        if (line->length) {
            console_erase_previous();
            line->length--;
        }
    } else if (c >= ' ' && c <= '~' && line->length < 128) {
        serial_putc(c);
        line->length++;
    }
}

void input_console_run(void)
{
    struct keyboard k;
    if (!keyboard_init(&k, &port_io)) {
        serial_puts("input: PS/2 initialization failed; console unavailable\r\n");
        return;
    }
    serial_puts("input: PS/2 ready, translated set 1, polling; IRQ masks unchanged\r\n");
    serial_puts("VOS input console: type text, Backspace edits, Enter starts a line.\r\n");
    serial_puts("Echo only; vsh is next. US keyboard, 128 characters per line.\r\n");
    screen_status(SCREEN_PASS, "keyboard input ready");
    serial_puts("input> ");
    struct input_console_line line = { 0 };
    for (;;) {
        char c = keyboard_poll(&k);
        input_console_accept(&line, c);
        /* The existing idle thread handles PIT interrupts. Kernel IF remains 0;
         * no busy wait, new ring-0 IRQ exception, or scheduler policy change.
         */
        thread_sleep(1);
    }
}
