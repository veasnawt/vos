/* R4-V1: canonical, check_va, check_nonleaf (vmm.c) and user_range (usercopy.c) against the frozen accepted C
 * (tests/reference/vmm-r4v1.c, usercopy-r4v1.c). scripts/test-vmm-check.py builds this file five times:
 *   -DSIDE=ref|prod -DSIDE_VMM -DSRC="<vmm source>"            the three static decisions of one vmm.c, exported as SIDE_*
 *   -DSIDE=ref|prod -DSIDE_USERCOPY -DSRC="<usercopy source>"  user_range and the three public user-copy functions
 *   (no SIDE)                                                  the comparison below
 * Every case compares three results exactly: the frozen C, the production C function (which forwards to Rust) and the Rust
 * export called directly. The user-copy callers are compared on their result and on the exact sequence of vmm_lookup
 * calls they make against a stub that refuses one chosen page, so no copy runs on the host.
 * usage: vmm-check-differential <random cases> */
#include <stdint.h>
#include <stddef.h>
#define CAT_(a, b) a##_##b
#define CAT(a, b) CAT_(a, b)
#ifdef SIDE_VMM
#include SRC
int CAT(SIDE, canonical)(uint64_t va) { return canonical(va); }
int CAT(SIDE, check_va)(uint64_t va) { return check_va(va); }
int CAT(SIDE, check_nonleaf)(uint64_t e, int level) { return check_nonleaf(e, level); }
#elif defined(SIDE_USERCOPY)
#include SRC
int CAT(SIDE, user_range)(uint64_t uptr, size_t len) { return user_range(uptr, len); }
#else
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "memlayout.h"
#include "vmm.h"
#include "rust_kernel.h"

#define SIDES(ret, name, args) ret ref_##name args; ret prod_##name args
SIDES(int, canonical, (uint64_t)); SIDES(int, check_va, (uint64_t)); SIDES(int, check_nonleaf, (uint64_t, int)); SIDES(int, user_range, (uint64_t, size_t));
SIDES(int, user_writable, (uint64_t, uint64_t, size_t)); SIDES(int, copy_to_user, (uint64_t, uint64_t, const void *, size_t));
SIDES(int, copy_from_user, (uint64_t, void *, uint64_t, size_t));

static unsigned long long cases, mismatches, cov_canon[3], cov_va[5], cov_nl[4], cov_ur[5], cov_call[4];
static uint64_t digest = 1469598103934665603ull;
static void mix(uint64_t v) { for (int i = 0; i < 8; i++) { digest ^= (v >> (8 * i)) & 255; digest *= 1099511628211ull; } }
static void differ(const char *fn, uint64_t a, uint64_t b, long long r, long long p, long long d)
{
    if (mismatches++ < 20) printf("MISMATCH %s(%#llx, %#llx): frozen %lld production %lld rust %lld\n", fn, (unsigned long long)a, (unsigned long long)b, r, p, d);
}
#define CHECK(fn, a, b, r, p, d) do { cases++; mix((uint64_t)(int64_t)(r)); if ((r) != (p) || (r) != (d)) differ(fn, a, b, r, p, d); } while (0)

static void t_va(uint64_t va)
{
    int r = ref_canonical(va); CHECK("canonical", va, 0, r, prod_canonical(va), vos_rust_vmm_canonical(va));
    cov_canon[r ? (va >= UPPER_HALF_START ? 2 : 1) : 0]++;
    r = ref_check_va(va); CHECK("check_va", va, 0, r, prod_check_va(va), vos_rust_vmm_check_va(va));
    if (r >= 0 && r < 5) cov_va[r]++;
}
static void t_nl(uint64_t e, int level)
{
    int r = ref_check_nonleaf(e, level); CHECK("check_nonleaf", e, (uint64_t)(int64_t)level, r, prod_check_nonleaf(e, level), vos_rust_vmm_check_nonleaf(e, level));
    cov_nl[r == VMM_OK ? 0 : r == VMM_ELARGE ? 1 : (e & PTE_PS) ? 2 : 3]++;
}

/* The stub page-table lookup: records every call and refuses call number `fail_at` (and any eighth call), so the
 * callers' own page loops are observed but no copy to a kernel address can start. */
static uint64_t trace; static unsigned calls, fail_at, page_flags;
int vmm_lookup(uint64_t root, uint64_t va, struct vmm_mapping *m)
{
    trace = (trace ^ root ^ (va * 0x9e3779b97f4a7c15ull)) * 1099511628211ull;
    m->pa = 0x200000; m->page_size = PAGE_SIZE; m->flags = page_flags;
    return calls++ == fail_at || calls > 7 ? VMM_ENOTMAPPED : VMM_OK;
}
__attribute__((noreturn)) void panic(const char *s) { printf("MISMATCH unexpected panic: %s\n", s); exit(3); }

static void t_ur(uint64_t root, uint64_t uptr, size_t len, unsigned r3)
{
    static char buf[16];
    int r = ref_user_range(uptr, len), p = prod_user_range(uptr, len); CHECK("user_range", uptr, len, r, p, vos_rust_user_range(uptr, len));
    if (r != p) return;         /* already a mismatch: the callers would diverge (and a wrongly accepted range could reach the copy) */
    cov_ur[r ? (len ? 4 : 3) : uptr < USER_IMAGE_START ? 0 : uptr >= LOWER_HALF_END ? 1 : 2]++;
    /* the callers: same result and the same lookups, in the same order (zero-length shortcuts make none) */
    for (int f = 0; f < 3; f++) {
        uint64_t tr[2]; unsigned n[2]; int res[2];
        for (int side = 0; side < 2; side++) {
            trace = 0; calls = 0; fail_at = r3 % 3; page_flags = VMM_USER | ((r3 & 4) ? VMM_WRITE : 0);
            if (len && r && fail_at > ((uptr + len - 1) >> 12) - (uptr >> 12)) fail_at = 0;     /* always inside the range */
            res[side] = f == 0 ? (side ? prod_user_writable : ref_user_writable)(root, uptr, len)
                      : f == 1 ? (side ? prod_copy_to_user : ref_copy_to_user)(root, uptr, buf, len)
                               : (side ? prod_copy_from_user : ref_copy_from_user)(root, buf, uptr, len);
            tr[side] = trace; n[side] = calls;
        }
        cases++; mix((uint64_t)(int64_t)res[0]); mix(tr[0]); mix(n[0]);
        if (res[0] != res[1] || tr[0] != tr[1] || n[0] != n[1]) differ(f == 0 ? "user_writable" : f == 1 ? "copy_to_user" : "copy_from_user", uptr, len, res[0], res[1], n[1]);
        cov_call[len == 0 ? 0 : !r ? 1 : n[0] > 1 ? 3 : 2]++;
    }
}

static uint64_t seed = 0x52345631ull;       /* "R4V1" */
static uint64_t rnd(void) { uint64_t z = (seed += 0x9e3779b97f4a7c15ull); z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; z = (z ^ (z >> 27)) * 0x94d049bb133111ebull; return z ^ (z >> 31); }

static const uint64_t EDGE[] = {0, PAGE_SIZE, NULL_GUARD_END, LOWER_MAP_END, LOWER_HALF_END, 1ull << 63, UPPER_HALF_START, 0xffffffff80000000ull};
static const int LEVEL[] = {INT_MIN, -1, 0, 1, 2, 3, 4, 7, INT_MAX};
#define N(a) (sizeof(a) / sizeof(a)[0])
static uint64_t len_case(uint64_t uptr, unsigned i)
{
    const uint64_t room = LOWER_HALF_END - uptr;        /* wraps for uptr above the range: still a boundary worth trying */
    const uint64_t v[] = {0, 1, 2, PAGE_SIZE - 1, PAGE_SIZE, PAGE_SIZE + 1, room - 1, room, room + 1, 0xffffffffull, 1ull << 32, LOWER_HALF_END, 1ull << 63, ~0ull - 1, ~0ull};
    return v[i % N(v)];
}

int main(int argc, char **argv)
{
    unsigned long long random_cases = argc > 1 ? strtoull(argv[1], 0, 10) : 0;
    /* deterministic, exhaustive over the small domains */
    for (unsigned i = 0; i < N(EDGE); i++)                              /* every address within two pages of each boundary */
        for (int64_t d = -0x2100; d <= 0x2100; d++) t_va(EDGE[i] + (uint64_t)d);
    for (uint64_t va = 0; va < NULL_GUARD_END + 16 * PAGE_SIZE; va += PAGE_SIZE) t_va(va);     /* every page of the null guard and the first user pages */
    for (uint64_t va = LOWER_MAP_END - 16 * PAGE_SIZE; va <= LOWER_HALF_END; va += PAGE_SIZE) t_va(va);   /* the last lower-half pages */
    for (int a = 0; a < 64; a++) {                                      /* every one- and two-bit address and its complement */
        t_va(1ull << a); t_va(~(1ull << a));
        for (int b = a + 1; b < 64; b++) { t_va(1ull << a | 1ull << b); t_va(~(1ull << a | 1ull << b)); }
    }
    for (uint64_t low = 0; low < 4096; low++)                           /* every flag combination in bits 0..11 ... */
        for (int nx = 0; nx < 2; nx++)                                  /* ... with and without NX ... */
            for (int hi = 51; hi <= 63; hi++)                           /* ... none (51), each, or all (63) of the software/reserved bits 52..62 ... */
                for (int ad = 0; ad < 3; ad++) {                        /* ... and an empty, full or mixed address field, at every level */
                    uint64_t e = low | ((uint64_t)nx << 63) | (hi == 51 ? 0 : hi == 63 ? 0x7ffull << 52 : 1ull << hi) | (ad == 0 ? 0 : ad == 1 ? PTE_ADDR : 0x000a5a5a5a5a5000ull);
                    for (unsigned l = 0; l < N(LEVEL); l++) t_nl(e, LEVEL[l]);
                }
    for (unsigned i = 0; i < N(EDGE); i++)                              /* user ranges starting around each boundary, with every length class */
        for (int64_t d = -0x1010; d <= 0x1010; d += (d >= -0x40 && d < 0x40) ? 1 : 0x3f)
            for (unsigned l = 0; l < 15; l++) t_ur(0x1000 * (i + 1), EDGE[i] + (uint64_t)d, len_case(EDGE[i] + (uint64_t)d, l), (unsigned)(d + l));
    /* seeded, over the full 64-bit space: uniform values and values biased to the boundaries */
    for (unsigned long long i = 0; i < random_cases; i++) {
        uint64_t a = rnd(), b = rnd(), c = rnd();
        t_va(a); t_va(EDGE[b % N(EDGE)] + (uint64_t)(int16_t)c); t_va(a & ~0xfffull); t_va((a & 0x7fffffffffffull) & ~0xfffull);
        t_nl(a, (int)b); t_nl(a, (int)(b % 5)); t_nl((a & (PTE_P | PTE_W | PTE_U | PTE_A | PTE_ADDR | PTE_PS)) | ((c & 7) == 0 ? 1ull << (c >> 3 & 63) : 0), (int)(b % 5));
        t_ur(c, a, b, (unsigned)c); t_ur(c, a % LOWER_HALF_END, b % LOWER_HALF_END, (unsigned)(c >> 8));
        uint64_t u = USER_IMAGE_START + a % (LOWER_HALF_END - USER_IMAGE_START); t_ur(c, u, len_case(u, (unsigned)b), (unsigned)(c >> 16));
        t_ur(c, u, (LOWER_HALF_END - u) - (b & 3) + (c & 3), (unsigned)(c >> 24));
    }
    printf("VMM CHECK COVERAGE canonical=%llu,%llu,%llu check_va=%llu,%llu,%llu,%llu,%llu check_nonleaf=%llu,%llu,%llu,%llu user_range=%llu,%llu,%llu,%llu,%llu callers=%llu,%llu,%llu,%llu\n",
           cov_canon[0], cov_canon[1], cov_canon[2], cov_va[0], cov_va[1], cov_va[2], cov_va[3], cov_va[4], cov_nl[0], cov_nl[1], cov_nl[2], cov_nl[3],
           cov_ur[0], cov_ur[1], cov_ur[2], cov_ur[3], cov_ur[4], cov_call[0], cov_call[1], cov_call[2], cov_call[3]);
    printf("VMM CHECK DIFFERENTIAL cases=%llu mismatches=%llu digest=%016llx\n", cases, mismatches, (unsigned long long)digest);
    return mismatches ? 1 : 0;
}
#endif
