/* Private R0-B kernel bridge. This is not a syscall or application ABI. */
#ifndef VOS_X86_64_RUST_KERNEL_H
#define VOS_X86_64_RUST_KERNEL_H
#include <stdint.h>

struct keyboard;
/* R2 private synchronous driver bridge. Same thread/IF=0 ownership as
 * keyboard_poll; valid initialized keyboard and read callback required.
 * No Rust borrow spans the C io->in callback; no port writes or allocation. */
void vos_rust_keyboard_routed_drain(struct keyboard *keyboard);
/* Same live/IF=0 ownership; no reference spans the drain callback. */
char vos_rust_keyboard_routed_poll(struct keyboard *keyboard);
/* R2: ordinary (pointer_ready == 0) bounded polling; same live/IF=0 ownership,
 * at most 32 status+data pairs, no controller write, queue or allocation. */
char vos_rust_keyboard_ordinary_poll(struct keyboard *keyboard);
/* R2: bounded controller reads (keyboard / auxiliary channel). At most 100,000
 * status reads; ERR fails; other-channel bytes are consumed; *value is written
 * only when 1 is returned. Same live/IF=0 ownership; no controller write. */
int vos_rust_keyboard_read_keyboard(struct keyboard *keyboard, uint8_t *value);
int vos_rust_keyboard_read_aux(struct keyboard *keyboard, uint8_t *value);
struct pointer_packet;
/* R2: PS/2 pointer polling and packet dequeue Rust entry points. */
int vos_rust_keyboard_pointer_poll(struct keyboard *k, struct pointer_packet *p);
unsigned vos_rust_keyboard_pointer_lost(struct keyboard *k);

uint64_t vos_rust_probe(uint64_t seed, uint64_t salt);
/* R1: exactly 0 or 1; valid iff off <= size && len <= size - off. */
int vos_rust_elf_inside(uint64_t size, uint64_t off, uint64_t len);
static inline uint64_t rust_probe_expected(uint64_t seed, uint64_t salt)
{
    /* Unsigned C arithmetic specifies the same modulo-2^64 contract. */
    uint64_t left = (seed << 17) | (seed >> 47);
    uint64_t right = (salt >> 9) | (salt << 55);
    return (left + right) ^ UINT64_C(0x564f535255535430);
}
#ifdef VOS_TEST_CASE_rust_panic
__attribute__((noreturn)) void vos_rust_panic_test(void);
#endif
#endif
