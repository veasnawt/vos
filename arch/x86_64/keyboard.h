/* N4 first input: bounded i8042 polling, translated set-1 US keyboard.
 * Called from thread context with IF=0. No IRQ or syscall/ABI changes.
 * USB keyboards, layouts, LEDs and hotplug are not implemented yet.
 */
#ifndef VOS_X86_64_KEYBOARD_H
#define VOS_X86_64_KEYBOARD_H

#include <stdint.h>

#define KEYBOARD_WAIT_LIMIT 100000u
#define KEYBOARD_DRAIN_LIMIT 32u

struct keyboard_io {
    uint8_t (*in)(uint16_t port);
    void (*out)(uint16_t port, uint8_t value);
};

struct keyboard_decoder {
    uint8_t shift, ctrl, alt, caps, caps_down, extended, pause_left;
};

struct keyboard {
    const struct keyboard_io *io;
    struct keyboard_decoder decoder;
    int ready;
};

/* Pure decoder: returns printable ASCII, Enter ('\n'), Backspace ('\b'), or 0.
 * Release codes, unsupported keys and Ctrl/Alt combinations produce no text.
 */
char keyboard_decode(struct keyboard_decoder *d, uint8_t scan);
/* Configure the first port, explicitly select set 2 + controller translation.
 * Returns 1 on acknowledged initialization; 0 on a bounded timeout/error.
 * The auxiliary port is disabled: there is no mouse consumer in this increment.
 */
int keyboard_init(struct keyboard *k, const struct keyboard_io *io);
/* Nonblocking, at most KEYBOARD_DRAIN_LIMIT bytes; 0 if no character is ready. */
char keyboard_poll(struct keyboard *k);

/* Bounded foreground-session scan boundary; no bytes cross owners. */
int keyboard_suspend(struct keyboard *k);
int keyboard_resume(struct keyboard *k);

#endif
