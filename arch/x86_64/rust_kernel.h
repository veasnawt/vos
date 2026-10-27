/* Private R0-B kernel bridge. This is not a syscall or application ABI. */
#ifndef VOS_X86_64_RUST_KERNEL_H
#define VOS_X86_64_RUST_KERNEL_H
#include <stdint.h>

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
