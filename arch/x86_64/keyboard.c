#include "keyboard.h"
#include "rust_kernel.h"

/* Set-1 tables and keyboard_decode are Rust-owned.
 * R2 (complete for the PS/2 controller): every controller access is Rust-owned.
 * The bounded reads/writes (R2, R2-W) and the lifecycle sequences below (R2-L)
 * keep the accepted contracts; this file is the public C ABI shell and the
 * keyboard_poll mode dispatch. */

/* R2-L: initialization (port disable AD/A7, bounded drain, mode 0x20/0x60 with
 * (mode | 0x60) & ~0x13, AE, F5/F0/02/F4 with ACKs; ready only on success; no
 * output-port, A20, reset or PIC changes) is Rust-owned. */
int keyboard_init(struct keyboard *k, const struct keyboard_io *io)
{
    return vos_rust_keyboard_init(k, io);
}

static char routed_poll(struct keyboard *k);

/* Public dispatcher stays C: pointer_ready is read once to select the mode.
 * R2: the ordinary bounded polling loop is Rust-owned (same contract). */
char keyboard_poll(struct keyboard *k)
{
    if(k->pointer_ready)return routed_poll(k);
    return vos_rust_keyboard_ordinary_poll(k);
}

/* R2-L: the bounded foreground scan boundary (F5 + ACK search + drain; F4) is Rust-owned. */
int keyboard_suspend(struct keyboard *k)
{
    return vos_rust_keyboard_suspend(k);
}
int keyboard_resume(struct keyboard *k)
{
    return vos_rust_keyboard_resume(k);
}

/* One controller owner, two decoders. Never interpret auxiliary bytes as keys. */
/* R2: FIFO extraction policy is Rust-owned; public dispatcher stays C. */
static char routed_poll(struct keyboard *k) {
    return vos_rust_keyboard_routed_poll(k);
}
/* R2-L: pointer lease start (BAT 0xAA, ID 0x00, A7 rollback, AE/resume) and
 * best-effort stop are Rust-owned. */
int keyboard_pointer_start(struct keyboard *k) {
    return vos_rust_keyboard_pointer_start(k);
}
void keyboard_pointer_stop(struct keyboard *k) {
    vos_rust_keyboard_pointer_stop(k);
}
/* R2: bounded PS/2 pointer polling and loss accounting are Rust-owned; C ABI preserved. */
int keyboard_pointer_poll(struct keyboard *k, struct pointer_packet *p) {
    return vos_rust_keyboard_pointer_poll(k, p);
}
unsigned keyboard_pointer_lost(struct keyboard *k) {
    return vos_rust_keyboard_pointer_lost(k);
}
