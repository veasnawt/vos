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
/* R2-W bounded controller writes. write_port: at most 100,000 status (0x64) reads,
 * one write after IBF clears, none after timeout; only ports 0x60/0x64 (others
 * refused without access). command: 0x60 write then keyboard ACK 0xFA. aux_command:
 * 0xD4 to 0x64, byte to 0x60, then aux ACK 0xFA. Each stops at the first failure.
 * Same live/IF=0 ownership; io is reloaded for every access. */
int vos_rust_keyboard_write_port(struct keyboard *keyboard, uint16_t port, uint8_t value);
int vos_rust_keyboard_command(struct keyboard *keyboard, uint8_t value);
int vos_rust_keyboard_aux_command(struct keyboard *keyboard, uint8_t value);
struct keyboard_io;
/* R2-L controller lifecycle (exact accepted sequences; see keyboard.c). init accepts
 * writable storage whose contents are discarded; the others need a live object.
 * Same IF=0 ownership; no output-port, A20, reset, PIC or new commands. */
int vos_rust_keyboard_init(struct keyboard *keyboard, const struct keyboard_io *io);
int vos_rust_keyboard_suspend(struct keyboard *keyboard);
int vos_rust_keyboard_resume(struct keyboard *keyboard);
int vos_rust_keyboard_pointer_start(struct keyboard *keyboard);
void vos_rust_keyboard_pointer_stop(struct keyboard *keyboard);
/* R3-N1 private network bridge (Rust ARP/IPv4/UDP core). Same contracts as
 * net_arp.h/net_ipv4.h/net_udp.h, which keep the public ABI. */
#include <stddef.h>
struct net_arp; struct net_ipv4; struct net_udp; struct net_udp_datagram; struct net_ipv4_payload;
uint16_t vos_rust_net_checksum(const void *bytes, size_t length);
uint16_t vos_rust_net_transport_checksum(const uint8_t source[4], const uint8_t dest[4], uint8_t protocol, const void *bytes, size_t length);
int vos_rust_net_ethernet_type(const struct net_arp *a, const void *frame, size_t length, uint16_t *type);
int vos_rust_net_ipv4_payload(const struct net_ipv4 *c, const void *frame, size_t length, uint8_t protocol, struct net_ipv4_payload *view);
int vos_rust_net_ipv4_begin(const struct net_ipv4 *c, const uint8_t mac[6], const uint8_t ip[4], uint8_t protocol, size_t payload_length, void *out, size_t capacity, size_t *length);
int vos_rust_net_arp_init(struct net_arp *a, const uint8_t mac[6], const uint8_t ip[4]);
int vos_rust_net_neighbor_get(struct net_arp *a, const uint8_t ip[4], uint64_t now, uint8_t mac[6]);
int vos_rust_net_arp_query(struct net_arp *a, const uint8_t ip[4], uint64_t now, void *out, size_t capacity, size_t *length);
int vos_rust_net_ethernet_input(struct net_arp *a, const void *frame, size_t length, uint64_t now, void *out, size_t capacity, size_t *output_length);
int vos_rust_net_ipv4_init(struct net_ipv4 *c, const uint8_t mac[6], const uint8_t ip[4]);
int vos_rust_net_ipv4_echo_request(struct net_ipv4 *c, const uint8_t peer[4], uint16_t id, uint16_t sequence, const void *data, size_t data_length, uint64_t now, void *out, size_t capacity, size_t *length);
int vos_rust_net_ipv4_input(struct net_ipv4 *c, const void *frame, size_t length, uint64_t now, void *out, size_t capacity, size_t *output_length);
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
