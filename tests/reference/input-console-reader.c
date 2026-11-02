#include "input_console.h"
#include "console.h"
#include "keyboard.h"
#include "thread.h"
int input_console_read_line_for(struct keyboard *keyboard, char *out, unsigned capacity,
                                const char *prompt, int (*cancelled)(void *), void *ctx)
{
    if (!keyboard || !keyboard->ready)
        return INPUT_CONSOLE_UNAVAILABLE;
    if (!out || capacity < INPUT_CONSOLE_LINE_MAX + 1)
        return INPUT_CONSOLE_NOSPACE;
    struct input_console_line line = { 0 };
    serial_puts(prompt);
    for (;;) {
        if (cancelled && cancelled(ctx))
            return INPUT_CONSOLE_INTERRUPTED;
        input_console_accept(&line, keyboard_poll(keyboard));
        if (line.ready) {
            int result = input_console_take_line(&line, out, capacity);
            if (result != INPUT_CONSOLE_TOOLONG)
                return result;
            serial_puts("input: line too long; discarded (128 characters maximum)\r\n");
            serial_puts(prompt);
        }
        /* Only the existing idle thread enables interrupts for its PIT wait. */
        thread_sleep(1);
    }
}

