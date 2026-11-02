#include "input_console.h"
#include "console.h"
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

