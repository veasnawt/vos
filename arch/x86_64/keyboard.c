#include "keyboard.h"

#define DATA    0x60
#define STATUS  0x64
#define OBF     0x01
#define IBF     0x02
#define AUX     0x20
#define ERR     0xC0

static const char plain[128] = {
    [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4', [0x06]='5',
    [0x07]='6', [0x08]='7', [0x09]='8', [0x0A]='9', [0x0B]='0',
    [0x0C]='-', [0x0D]='=', [0x0E]='\b',
    [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t',
    [0x15]='y', [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p',
    [0x1A]='[', [0x1B]=']', [0x1C]='\n',
    [0x1E]='a', [0x1F]='s', [0x20]='d', [0x21]='f', [0x22]='g',
    [0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l', [0x27]=';',
    [0x28]='\'', [0x29]='`', [0x2B]='\\',
    [0x2C]='z', [0x2D]='x', [0x2E]='c', [0x2F]='v', [0x30]='b',
    [0x31]='n', [0x32]='m', [0x33]=',', [0x34]='.', [0x35]='/', [0x39]=' ',
};
static const char shifted[128] = {
    [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%',
    [0x07]='^', [0x08]='&', [0x09]='*', [0x0A]='(', [0x0B]=')',
    [0x0C]='_', [0x0D]='+', [0x1A]='{', [0x1B]='}', [0x27]=':',
    [0x28]='"', [0x29]='~', [0x2B]='|', [0x33]='<', [0x34]='>', [0x35]='?',
};

char keyboard_decode(struct keyboard_decoder *d, uint8_t scan)
{
    if (d->pause_left) {
        d->pause_left--;
        return 0;
    }
    if (scan == 0xE1) {                /* Pause: E1 1D 45 E1 9D C5 */
        d->extended = 0;
        d->pause_left = 5;
        return 0;
    }
    if (scan == 0xE0) {
        d->extended = 1;
        return 0;
    }
    int ext = d->extended, up = scan & 0x80;
    unsigned code = scan & 0x7F;
    uint8_t bit = ext ? 2 : 1;
    d->extended = 0;
    if (code == 0x1D || code == 0x38) {
        uint8_t *mask = code == 0x1D ? &d->ctrl : &d->alt;
        *mask = up ? (uint8_t)(*mask & ~bit) : (uint8_t)(*mask | bit);
        return 0;
    }
    if (ext)                           /* arrows, keypad Enter, PrintScreen's fake Shift */
        return 0;
    if (code == 0x2A || code == 0x36) {
        bit = code == 0x2A ? 1 : 2;
        d->shift = up ? (uint8_t)(d->shift & ~bit) : (uint8_t)(d->shift | bit);
        return 0;
    }
    if (code == 0x3A) {
        if (!up && !d->caps_down)
            d->caps ^= 1;
        d->caps_down = !up;
        return 0;
    }
    if (up || d->ctrl || d->alt)
        return 0;
    char c = plain[code];
    if (c >= 'a' && c <= 'z')
        return (!!d->shift ^ d->caps) ? (char)(c - 'a' + 'A') : c;
    return d->shift && shifted[code] ? shifted[code] : c;
}

static int write_port(struct keyboard *k, uint16_t port, uint8_t value)
{
    for (unsigned i = 0; i < KEYBOARD_WAIT_LIMIT; i++)
        if (!(k->io->in(STATUS) & IBF)) {
            k->io->out(port, value);
            return 1;
        }
    return 0;
}

static int read_keyboard(struct keyboard *k, uint8_t *value)
{
    for (unsigned i = 0; i < KEYBOARD_WAIT_LIMIT; i++) {
        uint8_t status = k->io->in(STATUS);
        if (!(status & OBF))
            continue;
        uint8_t data = k->io->in(DATA);
        if (status & ERR)
            return 0;
        if (!(status & AUX)) {
            *value = data;
            return 1;
        }
    }
    return 0;
}

static int command(struct keyboard *k, uint8_t value)
{
    uint8_t ack;
    return write_port(k, DATA, value) && read_keyboard(k, &ack) && ack == 0xFA;
}

int keyboard_init(struct keyboard *k, const struct keyboard_io *io)
{
    *k = (struct keyboard){ .io = io };
    if (!io || !io->in || !io->out)
        return 0;
    if (!write_port(k, STATUS, 0xAD) || !write_port(k, STATUS, 0xA7))
        return 0;                       /* stop both ports before reading the mode byte */
    unsigned i;
    for (i = 0; i < KEYBOARD_DRAIN_LIMIT && (io->in(STATUS) & OBF); i++)
        (void)io->in(DATA);
    if (io->in(STATUS) & OBF)
        return 0;
    uint8_t mode;
    if (!write_port(k, STATUS, 0x20) || !read_keyboard(k, &mode))
        return 0;
    /* Keep system/reserved bits. Disable IRQ1/12 and aux clock; translate set 2.
     * No output-port commands (A20/reset) and no changes to PIC masks.
     */
    mode = (uint8_t)((mode | 0x60) & ~0x13);
    if (!write_port(k, STATUS, 0x60) || !write_port(k, DATA, mode) ||
        !write_port(k, STATUS, 0xAE) || !command(k, 0xF5) ||
        !command(k, 0xF0) || !command(k, 0x02) || !command(k, 0xF4))
        return 0;
    k->ready = 1;
    return 1;
}

char keyboard_poll(struct keyboard *k)
{
    if (!k->ready)
        return 0;
    for (unsigned i = 0; i < KEYBOARD_DRAIN_LIMIT; i++) {
        uint8_t status = k->io->in(STATUS);
        if (!(status & OBF))
            break;
        uint8_t data = k->io->in(DATA);
        if (status & AUX)
            continue;
        if (status & ERR || data == 0 || data == 0xFF) {
            k->decoder = (struct keyboard_decoder){ 0 }; /* lost stream: release modifiers */
            continue;
        }
        char c = keyboard_decode(&k->decoder, data);
        if (c)
            return c;
    }
    return 0;
}
