/* R4-M3: replay pmm_init_in calls recorded from a running VOS (scripts/test-pmm-init-native.py) through the FROZEN accepted C
 * (tests/reference/pmm-r4m3.c, public symbols renamed ref_). One call per input line: the path of a record file, whose layout is
 *   u64 frames, u64 has_map, u32 entry_size, u32 entry_version, u32 entries, u32 has_resv, then entries * entry_size map bytes,
 *   then (has_resv) the 1928-byte struct resv_table with its `what` pointers zeroed.
 * The frozen C runs on fresh, garbage-filled bitmaps of PMM_WORDS(frames) words. Prints "result fnv" per call: the return value
 * and a 64-bit FNV-1a of the instance after the call (without its two pointer fields) followed, when the call succeeded, by both
 * bitmaps; the script compares it with the guest's. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmm.h"
#include "multiboot2.h"

int ref_pmm_init_in(struct pmm *, uint64_t *, uint64_t *, uint64_t, const struct boot_info *, const struct resv_table *);
__attribute__((noreturn)) void panic(const char *s) { fprintf(stderr, "unexpected panic: %s\n", s); abort(); }
const struct mb2_mmap_entry *ref_boot_mmap_entry(const struct boot_info *bi, uint32_t i)
{ return (const struct mb2_mmap_entry *)((const uint8_t *)bi->mmap + sizeof *bi->mmap + (size_t)i * bi->mmap->entry_size); }

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

int main(void)
{
    static char path[4096];
    static uint64_t avail[PMM_WORDS(PMM_MAX_FRAMES)], alloc[PMM_WORDS(PMM_MAX_FRAMES)];
    static uint8_t tagbuf[16 + 64 * 1024] __attribute__((aligned(8)));
    static struct resv_table t;
    unsigned long long n = 0;
    while (scanf("%4095s", path) == 1) {
        FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "missing %s\n", path); return 2; }
        uint64_t frames, has_map; uint32_t esize, version, entries, has_resv;
        if (fread(&frames, 8, 1, f) != 1 || fread(&has_map, 8, 1, f) != 1 || fread(&esize, 4, 1, f) != 1 || fread(&version, 4, 1, f) != 1 ||
            fread(&entries, 4, 1, f) != 1 || fread(&has_resv, 4, 1, f) != 1) return 2;
        size_t mlen = (size_t)entries * esize;
        if (mlen > sizeof tagbuf - 16) return 2;
        memset(tagbuf, 0, sizeof tagbuf);
        struct mb2_tag_mmap *tag = (struct mb2_tag_mmap *)tagbuf;
        tag->tag.type = 6; tag->tag.size = (uint32_t)(16 + mlen); tag->entry_size = esize; tag->entry_version = version;
        if (has_map && mlen && fread(tagbuf + 16, 1, mlen, f) != mlen) return 2;
        if (has_resv && fread(&t, sizeof t, 1, f) != 1) return 2;
        fclose(f);
        struct boot_info bi; memset(&bi, 0, sizeof bi);
        bi.mmap = has_map ? tag : 0; bi.mmap_entries = entries;
        memset(avail, 0xa5, sizeof avail); memset(alloc, 0x5a, sizeof alloc);
        struct pmm p; memset(&p, 0xcc, sizeof p);
        int r = ref_pmm_init_in(&p, avail, alloc, frames, &bi, has_resv ? &t : 0);
        uint64_t h = fnv(1469598103934665603ull, (uint8_t *)&p + 16, 48);
        if (r == PMM_OK) { h = fnv(h, avail, PMM_WORDS(frames) * 8); h = fnv(h, alloc, PMM_WORDS(frames) * 8); }
        printf("%d %016llx\n", r, (unsigned long long)h); n++;
    }
    fprintf(stderr, "replayed %llu initializations\n", n);
    return 0;
}
