/* Execute the production decoder/initialization/poll and line editor on the host. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "keyboard.h"
#include "input_console.h"

static uint8_t data[256], flags[256], mode;
static unsigned head, tail, reads, writes, stage, commands;
static int stuck, no_reply, bad_ack, aux_flood;
static void enqueue(uint8_t byte, uint8_t status)
{
    assert(tail < sizeof(data));
    data[tail] = byte; flags[tail++] = status | 1;
}
static uint8_t port_in(uint16_t port)
{
    reads++;
    if (port == 0x64)
        return stuck ? 2 : head < tail ? flags[head] : aux_flood ? 0x21 : 0;
    assert(port == 0x60);
    return head < tail ? data[head++] : 0x1E;
}
static void port_out(uint16_t port, uint8_t value)
{
    writes++;
    if (port == 0x64) {
        assert(value == 0xAD || value == 0xA7 || value == 0x20 || value == 0x60 || value == 0xAE);
        if (value == 0x20 && !no_reply) enqueue(0x37, 0); /* old IRQs + disabled ports */
        if (value == 0x60) stage = 1;
    } else {
        assert(port == 0x60);
        if (stage) { mode = value; stage = 0; }
        else {
            static const uint8_t expected[] = { 0xF5, 0xF0, 0x02, 0xF4 };
            assert(commands < sizeof(expected) && value == expected[commands++]);
            if (!no_reply) enqueue(bad_ack ? 0xFE : 0xFA, 0);
        }
    }
}
static const struct keyboard_io io = { port_in, port_out };
static void reset(void)
{
    head=tail=reads=writes=stage=commands=0;
    stuck=no_reply=bad_ack=aux_flood=0;
    mode=0;
}

/* UI sinks let the host exercise the actual editor, not a copied model. */
static char output[2048];
static unsigned count, erased;
void serial_putc(char c) { assert(count < sizeof(output)); output[count++] = c; }
void serial_puts(const char *s) { while (*s) serial_putc(*s++); }
void console_erase_previous(void) { erased++; }

int main(void)
{
    struct keyboard_decoder d = { 0 };
    assert(keyboard_decode(&d, 0x1E) == 'a');
    assert(!keyboard_decode(&d, 0x9E));
    keyboard_decode(&d, 0x2A); keyboard_decode(&d, 0x36);
    keyboard_decode(&d, 0xAA); /* release one shift while the other remains held */
    assert(keyboard_decode(&d, 0x1E) == 'A');
    assert(keyboard_decode(&d, 0x02) == '!');
    keyboard_decode(&d, 0xB6);
    keyboard_decode(&d, 0x3A); keyboard_decode(&d, 0x3A); /* Caps repeat toggles once */
    assert(keyboard_decode(&d, 0x1E) == 'A');
    assert(keyboard_decode(&d, 0x02) == '1');
    keyboard_decode(&d, 0x2A);
    assert(keyboard_decode(&d, 0x1E) == 'a');
    keyboard_decode(&d, 0xAA); keyboard_decode(&d, 0xBA);
    keyboard_decode(&d, 0x3A); keyboard_decode(&d, 0xBA);
    keyboard_decode(&d, 0xE0); keyboard_decode(&d, 0x2A); /* PrintScreen fake shift */
    assert(!d.shift && keyboard_decode(&d, 0x1E) == 'a');
    keyboard_decode(&d, 0xE0); assert(!keyboard_decode(&d, 0x48)); /* arrow */
    const uint8_t pause[] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
    for (unsigned i=0; i<sizeof(pause); i++) assert(!keyboard_decode(&d, pause[i]));
    assert(!d.ctrl && !d.pause_left && keyboard_decode(&d, 0x1E) == 'a');
    keyboard_decode(&d, 0x1D);
    assert(!keyboard_decode(&d, 0x1E));
    keyboard_decode(&d, 0xE0); keyboard_decode(&d, 0x1D);
    keyboard_decode(&d, 0x9D); assert(!keyboard_decode(&d, 0x1E));
    keyboard_decode(&d, 0xE0); keyboard_decode(&d, 0x9D);
    assert(keyboard_decode(&d, 0x1E) == 'a');
    keyboard_decode(&d, 0x38); assert(!keyboard_decode(&d, 0x1E));
    keyboard_decode(&d, 0xB8);
    assert(keyboard_decode(&d, 0x1C) == '\n' && keyboard_decode(&d, 0x0E) == '\b');
    assert(!keyboard_decode(&d, 0x01) && !keyboard_decode(&d, 0x0F)); /* Esc, Tab */
    puts("PASS: decoder modifiers, repeat, release, extended/Pause and unsupported keys");

    struct keyboard k;
    reset(); assert(keyboard_init(&k, &io));
    assert(mode == 0x64 && commands == 4 && k.ready);
    enqueue(0x1E, 0x20); enqueue(0x2A, 0x80); enqueue(0x1E, 0);
    assert(keyboard_poll(&k) == 'a'); /* mouse ignored, corrupt modifier discarded */
    assert(!keyboard_poll(&k));
    for (unsigned i=0; i<KEYBOARD_DRAIN_LIMIT; i++) enqueue(0x9E, 0);
    enqueue(0x1E, 0);
    assert(!keyboard_poll(&k) && keyboard_poll(&k) == 'a');
    enqueue(0x2A, 0); enqueue(0x00, 0); enqueue(0x1E, 0);
    assert(keyboard_poll(&k) == 'a'); /* overflow/error resets sticky modifiers */
    reset(); stuck=1; assert(!keyboard_init(&k, &io) && !k.ready && !writes);
    assert(reads == KEYBOARD_WAIT_LIMIT);
    reset(); no_reply=1; assert(!keyboard_init(&k, &io) && !k.ready);
    assert(reads < KEYBOARD_WAIT_LIMIT+100);
    reset(); bad_ack=1; assert(!keyboard_init(&k, &io) && !k.ready && commands == 1);
    reset(); aux_flood=1; assert(!keyboard_init(&k, &io) && !k.ready);
    assert(reads < 2*KEYBOARD_DRAIN_LIMIT+10);
    assert(!keyboard_init(&k, NULL));
    assert(!keyboard_poll(&k));
    puts("PASS: acknowledged setup, bounded timeouts/drain, aux/errors and fail-closed readiness");

    assert(!input_console_requested(NULL) && !input_console_requested(""));
    assert(input_console_requested("vos.console=1"));
    assert(input_console_requested("other=1\tvos.console=1 other=2"));
    assert(!input_console_requested("xvos.console=1 vos.console=10 vos.console=1x"));
    struct input_console_line line = { 0 };
    input_console_accept(&line, '\b'); assert(!erased && !line.length);
    input_console_accept(&line, 'x'); input_console_accept(&line, '\b');
    assert(erased == 1 && !line.length);
    for (unsigned i=0; i<140; i++) input_console_accept(&line, 'a');
    assert(line.length == 128 && count == 129);
    input_console_accept(&line, '\t'); assert(count == 129);
    input_console_accept(&line, '\b'); assert(line.length == 127 && erased == 2);
    input_console_accept(&line, '\n');
    assert(line.ready && line.length == 127 && !memcmp(output+129, "\r\n", 2));
    char delivered[INPUT_CONSOLE_LINE_MAX + 1] = "unchanged";
    assert(input_console_take_line(&line, delivered, sizeof(delivered)) == INPUT_CONSOLE_TOOLONG);
    assert(!strcmp(delivered, "unchanged") && !line.ready && !line.length);
    puts("PASS: exact opt-in token, prompt boundary, 128-cell limit, editing, Enter and overflow rejection");
    return 0;
}
