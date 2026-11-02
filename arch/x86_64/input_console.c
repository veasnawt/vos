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

/* input_console_accept is Rust-owned behind input_console.h. */

/* input_console_take_line is Rust-owned behind input_console.h. */

int input_console_read_line(struct keyboard *keyboard, char *out, unsigned capacity)
{
    return input_console_read_line_for(keyboard, out, capacity, "input> ", 0, 0);
}

/* input_console_read_line_for is Rust-owned behind input_console.h. */

void input_console_run(void)
{
    struct keyboard k;
    if (!keyboard_init(&k, &port_io)) {
        serial_puts("input: PS/2 initialization failed; console unavailable\r\n");
        return;
    }
    serial_puts("input: PS/2 ready, translated set 1, polling; IRQ masks unchanged\r\n");
    screen_begin_input();
    screen_status(SCREEN_PASS, "buffered input ready");
    serial_puts("VOS buffered input console\r\n");
    serial_puts("Type text, Backspace edits, Enter submits a completed line.\r\n");
    serial_puts("128 characters maximum; overlong lines are discarded. vsh is next.\r\n\r\n");
    for (;;) {
        char command[INPUT_CONSOLE_LINE_MAX + 1];
        int length = input_console_read_line(&k, command, sizeof(command));
        if (length < 0)
            return;
        /* Demonstrate delivery to a consumer. No command parsing/execution yet. */
        serial_puts("submitted: ");
        serial_puts(command);
        serial_puts("\r\n");
    }
}
