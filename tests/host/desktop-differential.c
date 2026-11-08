/* R3-G1: the frozen accepted C (tests/reference/desktop-r3g1.c, symbols renamed ref_*) against the production Rust
 * desktop_requested()/desktop_compute() (the kernel archive, or an overflow-checked host build of the same sources).
 * Every command line is placed flush against a PROT_NONE guard page, so a read past its NUL by either side faults.
 * Every layout output is prefilled; the two sides must return the same value and leave byte-identical outputs,
 * including untouched outputs on refusal. Prints a transcript digest so release and sanitizer runs can be compared. */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "desktop.h"

_Static_assert(sizeof(struct desktop_layout) == 32, "desktop_layout size");
_Static_assert(_Alignof(struct desktop_layout) == 4, "desktop_layout align");
_Static_assert(offsetof(struct desktop_layout, x) == 0 && offsetof(struct desktop_layout, h) == 12, "desktop_layout offsets");
_Static_assert(offsetof(struct desktop_layout, text_x) == 16 && offsetof(struct desktop_layout, text_h) == 28, "desktop_layout offsets");

int ref_desktop_requested(const char *cmdline);
int ref_desktop_compute(uint32_t width, uint32_t height, struct desktop_layout *out);
/* The frozen reference also defines ref_desktop_paint/ref_desktop_bar, which are never called here. */
void screen_fill(const struct screen_surface *s, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) {(void)s;(void)x;(void)y;(void)w;(void)h;(void)c;abort();}
void screen_glyph(const struct screen_surface *s, uint32_t x, uint32_t y, char ch, uint32_t fg, uint32_t bg) {(void)s;(void)x;(void)y;(void)ch;(void)fg;(void)bg;abort();}

static uint8_t *page;
static size_t pagesize;
static uint64_t digest = 1469598103934665603ull, req_cases, req_true, cmp_cases, cmp_ok, mismatches;
static void mix(uint64_t v) { for (int i = 0; i < 8; i++) { digest ^= (v >> (8 * i)) & 0xff; digest *= 1099511628211ull; } }
static void fail(const char *what) { if (++mismatches <= 20) fprintf(stderr, "MISMATCH %s\n", what); }

static void req_bytes(const uint8_t *s, size_t n)
{
    if (n + 1 > pagesize) abort();
    uint8_t *at = page + pagesize - (n + 1);   /* NUL is the last readable byte */
    memcpy(at, s, n); at[n] = 0;
    int a = ref_desktop_requested((const char *)at), b = desktop_requested((const char *)at);
    req_cases++; req_true += a == 1; mix(0x5200 | (uint64_t)(a & 0xff) << 16 | (uint64_t)n << 24);
    if (a != b || (a != 0 && a != 1)) {
        char buf[200]; snprintf(buf, sizeof buf, "requested len=%zu ref=%d rust=%d", n, a, b); fail(buf);
    }
}
static void req(const char *s) { req_bytes((const uint8_t *)s, strlen(s)); }

static void cmp(uint32_t w, uint32_t h)
{
    struct desktop_layout a, b;
    memset(&a, 0xa5, sizeof a); memset(&b, 0xa5, sizeof b);
    int ra = ref_desktop_compute(w, h, &a), rb = desktop_compute(w, h, &b);
    cmp_cases++; cmp_ok += ra == 1; mix((uint64_t)w << 32 | h); mix((uint64_t)(uint32_t)ra);
    if (ra == 1) { mix(a.w); mix(a.h); mix(a.text_w); mix(a.text_h); }
    if (ra != rb || memcmp(&a, &b, sizeof a)) {
        char buf[200]; snprintf(buf, sizeof buf, "compute %u x %u ref=%d rust=%d", w, h, ra, rb); fail(buf);
    }
}

static uint64_t rng = 0x5233473144534b31ull;   /* "R3G1DSK1" */
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

int main(void)
{
    pagesize = (size_t)sysconf(_SC_PAGESIZE);
    page = mmap(0, pagesize * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED || mprotect(page + pagesize, pagesize, PROT_NONE)) return 2;

    /* desktop_requested: NULL, structured fields and separators, edits of the token, random bytes. */
    if (ref_desktop_requested(0) != 0 || desktop_requested(0) != 0) fail("requested NULL");
    static const char *frag[] = {"", "vos.desktop=1", "vos.desktop=10", "vos.desktop=0", "vos.desktop=", "vos.desktop",
                                 "xvos.desktop=1", "vos.desktop=1x", "VOS.DESKTOP=1", "vos.apps=1", "vos.gui=1", "=1",
                                 "vos.desktop=1vos.desktop=1"};
    static const char *sep[] = {" ", "\t", "  ", " \t", "\t ", "\n", "\r", "\v", "\f", "\xa0", ","};
    enum { NF = sizeof frag / sizeof *frag, NS = sizeof sep / sizeof *sep };
    char buf[512];
    for (int l = -1; l < NS; l++) for (int f1 = 0; f1 < NF; f1++) for (int t = -1; t < NS; t++) {
        snprintf(buf, sizeof buf, "%s%s%s", l < 0 ? "" : sep[l], frag[f1], t < 0 ? "" : sep[t]); req(buf);
        for (int s1 = 0; s1 < NS; s1++) for (int f2 = 0; f2 < NF; f2++) {
            snprintf(buf, sizeof buf, "%s%s%s%s%s", l < 0 ? "" : sep[l], frag[f1], sep[s1], frag[f2], t < 0 ? "" : sep[t]); req(buf);
        }
    }
    for (int l = 0; l < 3; l++) for (int f1 = 0; f1 < NF; f1++) for (int s1 = 0; s1 < 3; s1++) for (int f2 = 0; f2 < NF; f2++)
        for (int s2 = 0; s2 < 3; s2++) for (int f3 = 0; f3 < NF; f3++) {
            snprintf(buf, sizeof buf, "%s%s%s%s%s%s", sep[l], frag[f1], sep[s1], frag[f2], sep[s2], frag[f3]); req(buf);
        }
    static const char *ctx[] = {"%s", "a %s", "%s b", "\t%s\t", "vos.apps=1 %s vos.gui=1"};
    const char *tok = "vos.desktop=1"; size_t tn = strlen(tok);
    for (unsigned c = 0; c < sizeof ctx / sizeof *ctx; c++) {
        char edit[64];
        for (size_t i = 0; i < tn; i++) for (int v = 1; v < 256; v++) {          /* every substitution */
            memcpy(edit, tok, tn + 1); edit[i] = (char)v; snprintf(buf, sizeof buf, ctx[c], edit); req(buf);
        }
        for (size_t i = 0; i <= tn; i++) for (int v = 1; v < 256; v++) {         /* every insertion */
            memcpy(edit, tok, i); edit[i] = (char)v; memcpy(edit + i + 1, tok + i, tn - i + 1); snprintf(buf, sizeof buf, ctx[c], edit); req(buf);
        }
        for (size_t i = 0; i < tn; i++) {                                         /* every deletion and prefix */
            memcpy(edit, tok, i); memcpy(edit + i, tok + i + 1, tn - i); snprintf(buf, sizeof buf, ctx[c], edit); req(buf);
            memcpy(edit, tok, i); edit[i] = 0; snprintf(buf, sizeof buf, ctx[c], edit); req(buf);
        }
    }
    {   /* long lines up to the guard page */
        size_t n = pagesize - 1; uint8_t *big = malloc(n);
        memset(big, ' ', n); req_bytes(big, n);
        memcpy(big + n - tn, tok, tn); req_bytes(big, n);
        memset(big, 'x', n); memcpy(big + n - tn, tok, tn); req_bytes(big, n);
        free(big);
    }
    static const uint8_t alpha[] = "vos.desktop=1 \t\n0x";
    for (unsigned i = 0; i < 200000; i++) {
        uint8_t s[64]; size_t n = next() % 49;
        for (size_t k = 0; k < n; k++) {
            uint64_t r = next(); s[k] = (r & 1) ? alpha[(r >> 8) % (sizeof alpha - 1)] : (uint8_t)(1 + (r >> 16) % 255);
        }
        if (n >= tn && (next() & 3) == 0) { size_t at = next() % (n - tn + 1); memcpy(s + at, tok, tn); }
        req_bytes(s, n);
    }

    /* desktop_compute: exhaustive rows/columns around both bounds, a dense box at the minimum, random pairs, NULL. */
    static const uint32_t edge[] = {0, 1, 2, 47, 48, 103, 104, 155, 156, 479, 480, 481, 600, 639, 640, 641, 767, 768, 1080,
                                    2160, 8191, 8192, 8193, 65535, 65536, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu};
    enum { NE = sizeof edge / sizeof *edge };
    for (uint32_t v = 0; v <= 10000; v++) for (int e = 0; e < NE; e++) { cmp(v, edge[e]); cmp(edge[e], v); }
    for (uint32_t w = 600; w <= 700; w++) for (uint32_t h = 440; h <= 520; h++) cmp(w, h);
    for (uint32_t w = 8170; w <= 8210; w++) for (uint32_t h = 8170; h <= 8210; h++) cmp(w, h);
    for (int a = 0; a < NE; a++) for (int b = 0; b < NE; b++) cmp(edge[a], edge[b]);
    for (unsigned i = 0; i < 300000; i++) {
        uint64_t r = next(); uint32_t w = (uint32_t)r, h = (uint32_t)(r >> 32);
        if (i & 1) { w %= 9000; h %= 9000; }
        cmp(w, h);
    }
    for (int a = 0; a < NE; a++) for (int b = 0; b < NE; b++)
        if (ref_desktop_compute(edge[a], edge[b], 0) != 0 || desktop_compute(edge[a], edge[b], 0) != 0) fail("compute NULL output");

    printf("DESKTOP DIFFERENTIAL requested=%llu (true %llu) compute=%llu (accepted %llu) mismatches=%llu digest=%016llx\n",
           (unsigned long long)req_cases, (unsigned long long)req_true, (unsigned long long)cmp_cases, (unsigned long long)cmp_ok,
           (unsigned long long)mismatches, (unsigned long long)digest);
    return mismatches ? 1 : 0;
}
