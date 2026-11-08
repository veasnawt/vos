/* R4-M1: replay PMM check calls recorded from a running VOS (scripts/test-pmm-native.py) through the FROZEN accepted C
 * (tests/reference/pmm-r4m1.c, built with every public symbol renamed ref_). One call per input line, one result per
 * output line:
 *   F state frames usable free hint phys word avail_word alloc_word     pmm_free_check_in
 *   S state frames usable free hint phys word avail_word alloc_word     pmm_frame_state_in
 *   C state frames usable free hint avail.bin alloc.bin                 pmm_check_in
 * (numbers in hex; "-" for a bitmap word or file the guest instance did not let the C read). The instance is rebuilt
 * with bitmaps of exactly PMM_WORDS(frames) words: for F/S zero except the recorded word, which is the only word the
 * frozen C reads; for C the complete recorded bitmaps. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmm.h"

int ref_pmm_free_check_in(const struct pmm *, uint64_t);
int ref_pmm_frame_state_in(const struct pmm *, uint64_t);
int ref_pmm_check_in(const struct pmm *);
__attribute__((noreturn)) void panic(const char *s) { fprintf(stderr, "unexpected panic: %s\n", s); abort(); }
const struct mb2_mmap_entry *ref_boot_mmap_entry(const struct boot_info *bi, uint32_t i) { (void)bi; (void)i; abort(); }

static uint64_t hex(const char *s, int *dash) { if (dash) *dash = s[0] == '-'; return s[0] == '-' ? 0 : strtoull(s, 0, 16); }
static int load(const char *path, uint64_t *w, uint64_t words)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(w, 8, words, f); int extra = fgetc(f) != EOF; fclose(f);
    return n == words && !extra;
}

int main(void)
{
    static char kind[4], a[9][4096];   /* at most 9 fields after the kind */
    static uint64_t avail[PMM_WORDS(PMM_MAX_FRAMES)], alloc[PMM_WORDS(PMM_MAX_FRAMES)];
    unsigned long long n = 0;
    while (scanf("%3s", kind) == 1) {
        struct pmm p; memset(&p, 0, sizeof p);
        int cnt = kind[0] == 'C' ? 7 : 9;
        for (int i = 0; i < cnt; i++) if (scanf("%4095s", a[i]) != 1) { fprintf(stderr, "short line %llu\n", n); return 2; }
        p.state = (int)hex(a[0], 0); p.frames = hex(a[1], 0); p.usable = hex(a[2], 0); p.free = hex(a[3], 0); p.hint = hex(a[4], 0);
        uint64_t words = p.frames / 64;
        int bounded = p.frames && p.frames % 64 == 0 && p.frames <= PMM_MAX_FRAMES;
        memset(avail, 0, sizeof avail); memset(alloc, 0, sizeof alloc);
        p.avail = bounded ? avail : 0; p.alloc = bounded ? alloc : 0;
        int r;
        if (kind[0] == 'C') {
            if (strcmp(a[5], "-")) {
                if (!bounded || !load(a[5], avail, words) || !load(a[6], alloc, words)) { fprintf(stderr, "bad bitmap files line %llu\n", n); return 2; }
            }
            r = ref_pmm_check_in(&p);
        } else {
            uint64_t phys = hex(a[5], 0); int d1, d2, d3;
            uint64_t w = hex(a[6], &d1), aw = hex(a[7], &d2), lw = hex(a[8], &d3);
            if (!d1) {
                if (!bounded || w >= words || d2 || d3) { fprintf(stderr, "bad word line %llu\n", n); return 2; }
                avail[w] = aw; alloc[w] = lw;
            }
            r = kind[0] == 'F' ? ref_pmm_free_check_in(&p, phys) : ref_pmm_frame_state_in(&p, phys);
        }
        printf("%d\n", r); n++;
    }
    fprintf(stderr, "replayed %llu calls\n", n);
    return 0;
}
