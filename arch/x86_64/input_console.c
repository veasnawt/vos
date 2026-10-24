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
    if (line->ready)
        return;
    if (c == '\n') {
        serial_puts("\r\n");
        line->ready = 1;
    } else if (c == '\b') {
        if (line->length) {
            console_erase_previous();
            line->bytes[--line->length] = 0;
        }
    } else if (c >= ' ' && c <= '~') {
        if (line->length == INPUT_CONSOLE_LINE_MAX) {
            line->overflow = 1;         /* never deliver a silently truncated command */
        } else {
            line->bytes[line->length++] = c;
            line->bytes[line->length] = 0;
            serial_putc(c);
        }
    }
}

int input_console_take_line(struct input_console_line *line, char *out, unsigned capacity)
{
    if (!line->ready)
        return INPUT_CONSOLE_PENDING;
    if (line->overflow) {
        *line = (struct input_console_line){ 0 };
        return INPUT_CONSOLE_TOOLONG;
    }
    if (!out || capacity <= line->length)
        return INPUT_CONSOLE_NOSPACE;
    unsigned length = line->length;
    for (unsigned i = 0; i <= length; i++)
        out[i] = line->bytes[i];
    *line = (struct input_console_line){ 0 };
    return (int)length;
}

int input_console_read_line(struct keyboard *keyboard, char *out, unsigned capacity)
{
    if (!keyboard || !keyboard->ready)
        return INPUT_CONSOLE_UNAVAILABLE;
    if (!out || capacity < INPUT_CONSOLE_LINE_MAX + 1)
        return INPUT_CONSOLE_NOSPACE;
    struct input_console_line line = { 0 };
    serial_puts("input> ");
    for (;;) {
        input_console_accept(&line, keyboard_poll(keyboard));
        if (line.ready) {
            int result = input_console_take_line(&line, out, capacity);
            if (result != INPUT_CONSOLE_TOOLONG)
                return result;
            serial_puts("input: line too long; discarded (128 characters maximum)\r\ninput> ");
        }
        /* Only the existing idle thread enables interrupts for its PIT wait. */
        thread_sleep(1);
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
