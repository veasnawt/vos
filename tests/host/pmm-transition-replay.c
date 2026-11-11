/* R4-M2: replay PMM transitions recorded from a running VOS (scripts/test-pmm-transition-native.py) through the FROZEN accepted C
 * (tests/reference/pmm-r4m2.c, public symbols renamed ref_; its pmm_free_in calls the accepted R4-M1 Rust check, linked from the
 * production Rust). One call per input line:
 *   A pre.bin           pmm_alloc_in
 *   F pre.bin phys      pmm_free_in
 *   R pre.bin phys      pmm_reserve_in
 * pre.bin is the guest's struct pmm (64 bytes) followed by PMM_WORDS(frames) avail words and as many alloc words (none when the
 * instance's bitmaps were not readable). Prints "result out fnv" per call: the return value, the output word (alloc) and a
 * 64-bit FNV-1a of the post-state (the struct without its two pointer fields, then both bitmaps), which the script compares
 * with the guest's post-state. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmm.h"

int ref_pmm_alloc_in(struct pmm *, uint64_t *);
int ref_pmm_free_in(struct pmm *, uint64_t);
int ref_pmm_reserve_in(struct pmm *, uint64_t);
__attribute__((noreturn)) void panic(const char *s) { fprintf(stderr, "unexpected panic: %s\n", s); abort(); }
const struct mb2_mmap_entry *ref_boot_mmap_entry(const struct boot_info *bi, uint32_t i) { (void)bi; (void)i; abort(); }

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

int main(void)
{
    static char kind[4], path[4096], arg[64];
    static uint64_t avail[PMM_WORDS(PMM_MAX_FRAMES)], alloc[PMM_WORDS(PMM_MAX_FRAMES)];
    unsigned long long n = 0;
    while (scanf("%3s %4095s", kind, path) == 2) {
        uint64_t phys = 0;
        if (kind[0] != 'A') { if (scanf("%63s", arg) != 1) return 2; phys = strtoull(arg, 0, 16); }
        FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "missing %s\n", path); return 2; }
        struct pmm p; uint8_t raw[64];
        if (fread(raw, 1, 64, f) != 64) return 2;
        memcpy(&p, raw, sizeof p);
        uint64_t words = p.frames / 64;
        if (words > PMM_WORDS(PMM_MAX_FRAMES)) words = 0;
        size_t got = words ? fread(avail, 8, words, f) : 0;
        int have = words && got == words && fread(alloc, 8, words, f) == words;
        fclose(f);
        if (have) { p.avail = avail; p.alloc = alloc; } else { p.avail = p.alloc = 0; words = 0; }
        uint64_t out = 0x5a5a5a5a5a5a5a5aull; int r;
        if (kind[0] == 'A') r = ref_pmm_alloc_in(&p, &out);
        else if (kind[0] == 'F') r = ref_pmm_free_in(&p, phys);
        else r = ref_pmm_reserve_in(&p, phys);
        memcpy(raw, &p, sizeof p);
        uint64_t h = fnv(1469598103934665603ull, raw + 16, 48);
        h = fnv(h, avail, words * 8); h = fnv(h, alloc, words * 8);
        printf("%d %llx %016llx\n", r, (unsigned long long)(kind[0] == 'A' ? out : 0), (unsigned long long)h); n++;
    }
    fprintf(stderr, "replayed %llu transitions\n", n);
    return 0;
}
