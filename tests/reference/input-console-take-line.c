#include "input_console.h"

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

