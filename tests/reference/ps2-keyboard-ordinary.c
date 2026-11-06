#include "keyboard.h"
#include "rust_kernel.h"

#define DATA    0x60
#define STATUS  0x64
#define OBF     0x01
#define IBF     0x02
#define AUX     0x20
#define ERR     0xC0

/* Set-1 tables and keyboard_decode are Rust-owned; hardware stays here. */

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

static char routed_poll(struct keyboard *k);

char keyboard_poll(struct keyboard *k)
{
    if(k->pointer_ready)return routed_poll(k);
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

int keyboard_suspend(struct keyboard *k)
{
    if(!k || !k->io)return 0;
    if(!k->ready)return 1;
    k->ready=0;
    if(!write_port(k,DATA,0xF5))return 0;
    int ack=0;
    for(unsigned i=0;i<KEYBOARD_DRAIN_LIMIT;i++) {
        uint8_t data;
        if(!read_keyboard(k,&data))break;
        if(data==0xFA){ack=1;break;}
    }
    if(!ack)return 0;
    for(unsigned i=0;i<KEYBOARD_DRAIN_LIMIT;i++) {
        if(!(k->io->in(STATUS)&OBF)) {
            k->decoder=(struct keyboard_decoder){0};
            return 1;
        }
        (void)k->io->in(DATA);
    }
    return 0;
}
int keyboard_resume(struct keyboard *k)
{
    if(!k || !k->io || k->ready)return 0;
    k->decoder=(struct keyboard_decoder){0};
    if(!command(k,0xF4))return 0;
    k->ready=1;
    return 1;
}

/* One controller owner, two decoders. Never interpret auxiliary bytes as keys. */
/* R2: FIFO extraction policy is Rust-owned; public dispatcher stays C. */
static char routed_poll(struct keyboard *k) {
    return vos_rust_keyboard_routed_poll(k);
}
static int read_aux(struct keyboard *k,uint8_t *b) {
    for(unsigned i=0;i<KEYBOARD_WAIT_LIMIT;i++) {
        uint8_t s=k->io->in(STATUS);
        if(!(s&OBF))continue;
        uint8_t data=k->io->in(DATA);
        if(s&ERR)return 0;
        if(s&AUX){*b=data;return 1;}
    }
    return 0;
}
static int aux_command(struct keyboard *k,uint8_t command_byte) {
    uint8_t b;
    return write_port(k,STATUS,0xd4) && write_port(k,DATA,command_byte) &&
        read_aux(k,&b) && b==0xfa;
}
int keyboard_pointer_start(struct keyboard *k) {
    if(!k || !k->ready || k->pointer_ready)return 0;
    if(!keyboard_suspend(k))return 0;
    uint8_t bat=0,id=255;
    int ok=write_port(k,STATUS,0xad) && write_port(k,STATUS,0xa8) &&
        aux_command(k,0xff) && read_aux(k,&bat) && bat==0xaa &&
        read_aux(k,&id) && id==0 && aux_command(k,0xf6) &&
        aux_command(k,0xe6) && aux_command(k,0xf4);
    if(!ok)(void)write_port(k,STATUS,0xa7);
    if(!write_port(k,STATUS,0xae) || !keyboard_resume(k))ok=0;
    if(!ok){(void)write_port(k,STATUS,0xa7);return 0;}
    k->pointer=(struct pointer_decoder){0};k->pointer_pending=0;k->pointer_lost=0;
    k->key_head=k->key_count=0;k->pointer_ready=1;return 1;
}
void keyboard_pointer_stop(struct keyboard *k) {
    if(!k || !k->pointer_ready)return;
    /* Stop keyboard scan generation while consuming auxiliary acknowledgments.
     * A final hardware drain is part of the subsequent keyboard_suspend boundary. */
    (void)write_port(k,STATUS,0xad);
    (void)aux_command(k,0xf5);
    (void)write_port(k,STATUS,0xa7);
    (void)write_port(k,STATUS,0xae);
    k->pointer_ready=k->pointer_pending=0;k->pointer=(struct pointer_decoder){0};
    k->pointer_lost=k->key_head=k->key_count=0;
}
/* R2: bounded PS/2 pointer polling and loss accounting are Rust-owned; C ABI preserved. */
int keyboard_pointer_poll(struct keyboard *k, struct pointer_packet *p) {
    return vos_rust_keyboard_pointer_poll(k, p);
}
unsigned keyboard_pointer_lost(struct keyboard *k) {
    return vos_rust_keyboard_pointer_lost(k);
}
