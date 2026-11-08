/* R3-G2: the frozen accepted screen.c (tests/reference/screen-r3g2.c, side "ref_") against the production screen.c with
 * its Rust geometry/bounds decisions (side "cand_"), as whole modules. Both sides run the same seeded operation stream
 * (screen_init, console output with wrap/scroll/erase, input mode, status, desktop, GUI sink and rows, pointer
 * begin/move/end, direct fills and glyphs on the framebuffer and on other surfaces, panic) on their own framebuffers
 * and VGA cell arrays, each flush against PROT_NONE guard pages. After every operation the return values, the module
 * state and every framebuffer/VGA byte must be identical. screen_choose is also compared exhaustively over its inputs.
 * Prints a transcript digest so release and sanitizer runs can be compared. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "bootinfo.h"
#include "screen.h"
#include "screen-diff.h"

#define SIDE(P) \
    int P##screen_kind(void); const char *P##screen_describe(void); void P##screen_init(const struct boot_info *); \
    void P##screen_putc(char); void P##screen_erase_previous(void); void P##screen_begin_input(void); void P##screen_restart_log(void); \
    int P##screen_desktop_begin(void); void P##screen_status(int, const char *); void P##screen_panic(const char *); \
    void P##screen_fill(const struct screen_surface *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t); \
    void P##screen_glyph(const struct screen_surface *, uint32_t, uint32_t, char, uint32_t, uint32_t); \
    int P##screen_gui_available(void); int P##screen_gui_begin(uint32_t, uint32_t); void P##screen_gui_row(uint32_t, const uint32_t *, uint32_t); \
    void P##screen_gui_end(void); uint32_t P##screen_pointer_begin(void); uint32_t P##screen_pointer_move(int32_t, int32_t); void P##screen_pointer_end(void); \
    void P##reset(void); void P##set_fb(uint32_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t); void P##set_vga(uint16_t *); \
    void P##set_pos(uint32_t, uint32_t); const struct screen_surface *P##fb(void); void P##state(struct screen_state *);
SIDE(ref_)
SIDE(cand_)
int ref_screen_choose(uint32_t, const char *, int, int, uint8_t, uint16_t, const char **);

/* ---- shared platform stubs (each side gets its own framebuffer through devmap_map) ---- */
static int side_now;
static uint32_t *fb_mem[2];
static struct boot_fb fb_cfg;
static const char *fb_err_cfg;
static int devmap_err_cfg;
void serial_puts(const char *s) { (void)s; }
void serial_hex(uint64_t v) { (void)v; }
void serial_dec(uint64_t v) { (void)v; }
/* Both sides are built with -Dboot_framebuffer=test_boot_framebuffer: the kernel archive's Rust boot_framebuffer is A3's. */
const char *test_boot_framebuffer(const struct boot_info *bi, struct boot_fb *f) { (void)bi; *f = fb_cfg; return fb_err_cfg; }
int devmap_map(uint64_t pa, uint64_t len, int attr, const char *what, void **va, int *used)
{
    (void)pa; (void)len; (void)what;
    if (devmap_err_cfg) return devmap_err_cfg;
    *va = fb_mem[side_now]; if (used) *used = attr; return 0;
}
const char *devmap_strerror(int err) { return err == 3 ? "window exhausted" : "mapping refused"; }
int devmap_mtrr_type(uint64_t pa) { (void)pa; return 1; }

static uint64_t digest = 1469598103934665603ull, ops, checks, mismatches;
/* Coverage of the decisions: every class must be reached (the gate pins minimums). */
static uint64_t cov_desktop, cov_gui, cov_inside, cov_outside, cov_drawn, cov_input, cov_vga, cov_scroll, cov_ring_wrap, cov_erase_back, cov_init_fb, cov_init_refused;
static void mix(uint64_t v) { for (int i = 0; i < 8; i++) { digest ^= (v >> (8 * i)) & 0xff; digest *= 1099511628211ull; } }
static void fail(const char *what, unsigned long long op)
{
    if (++mismatches <= 20) fprintf(stderr, "MISMATCH %s at op %llu\n", what, op);
}
static uint64_t rng = 0x5233473253435231ull;   /* "R3G2SCR1" */
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint32_t pick(const uint32_t *v, unsigned n) { return v[next() % n]; }

/* A pixel/cell region flush against a PROT_NONE guard page after it (and one before), so any write outside faults. */
static void *guarded(size_t bytes, size_t *mapped, void **map_base)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE), body = (bytes + page - 1) / page * page;
    uint8_t *m = mmap(0, body + 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED || mprotect(m, page, PROT_NONE) || mprotect(m + page + body, page, PROT_NONE)) abort();
    *mapped = body + 2 * page; *map_base = m;
    return m + page + body - bytes;
}

struct pair { uint32_t *px[2]; uint16_t *vga[2]; uint32_t *other[2]; size_t fb_bytes, other_bytes; void *maps[6]; size_t sizes[6];
              uint32_t w, h, pitch, ow, oh, opitch; };

static void compare(struct pair *p, unsigned long long op)
{
    struct screen_state a, b;
    ref_state(&a); cand_state(&b); checks++;
    if (memcmp(&a, &b, sizeof a)) fail("module state", op);
    if (p->fb_bytes && memcmp(p->px[0], p->px[1], p->fb_bytes)) fail("framebuffer bytes", op);
    if (memcmp(p->vga[0], p->vga[1], 80 * 25 * 2)) fail("VGA cells", op);
    if (memcmp(p->other[0], p->other[1], p->other_bytes)) fail("other surface bytes", op);
    cov_drawn += a.cur_drawn != 0; cov_input += a.input_mode != 0; cov_vga += a.kind == SCREEN_VGA_TEXT;
    mix(a.cx | (uint64_t)a.cy << 32); mix(a.cols | (uint64_t)a.rows << 32); mix(a.cur_x | (uint64_t)a.cur_y << 32); mix(a.cells_hash);
    mix(a.gui_x | (uint64_t)a.gui_y << 32); mix((uint64_t)a.kind << 8 | a.input_mode << 4 | a.desktop_mode << 2 | a.gui_active);
}

static void choose_exhaustive(void)
{
    static const uint32_t tags[] = {0, 1, 2, 0xffffffffu};
    static const char *errs[] = {NULL, "framebuffer is not 32 bits per pixel"};
    static const int kinds[] = {-1, 0, 1, 2, 3, 0x7fffffff};
    static const int efis[] = {0, 1, 2, -1};
    static const uint16_t bcols[] = {0, 40, 79, 80, 81, 160, 0xffff};
    for (unsigned t = 0; t < 4; t++) for (unsigned e = 0; e < 2; e++) for (unsigned k = 0; k < 6; k++) for (unsigned f = 0; f < 4; f++)
        for (unsigned m = 0; m < 256; m++) for (unsigned c = 0; c < 7; c++) {
            const char *wa = (const char *)0x1, *wb = (const char *)0x2;
            int ka = ref_screen_choose(tags[t], errs[e], kinds[k], efis[f], (uint8_t)m, bcols[c], &wa);
            int kb = screen_choose(tags[t], errs[e], kinds[k], efis[f], (uint8_t)m, bcols[c], &wb);
            ops++; mix((uint64_t)ka << 32 | strlen(wa));
            if (ka != kb || !wa || !wb || strcmp(wa, wb) || ((wa == errs[e]) != (wb == errs[e]))) fail("screen_choose", ops);
        }
}

int main(int argc, char **argv)
{
    unsigned scenarios = argc > 1 ? (unsigned)atoi(argv[1]) : 420, per = argc > 2 ? (unsigned)atoi(argv[2]) : 260;
    choose_exhaustive();
    static const uint32_t widths[] = {8, 15, 16, 31, 32, 33, 40, 64, 100, 159, 160, 255, 256, 300, 512, 639, 640, 641, 800, 1024, 1280};
    static const uint32_t heights[] = {15, 16, 17, 30, 44, 61, 62, 79, 80, 120, 160, 199, 200, 300, 479, 480, 481, 600, 768, 800};
    static const char *texts[] = {NULL, "", "ok", "Common payload: shell reclaimed, catalog retained", "a very long status text that cannot possibly fit in the bar at small widths"};
    for (unsigned sc = 0; sc < scenarios; sc++) {
        struct pair p; memset(&p, 0, sizeof p);
        int big = sc % 3 == 0;   /* a third of the scenarios use desktop-capable sizes */
        p.w = big ? pick(widths + 14, 7) : pick(widths, 21); p.h = big ? pick(heights + 14, 6) : pick(heights, 20);
        p.pitch = p.w + (uint32_t)(next() % 3 == 0 ? next() % 24 : 0);
        p.fb_bytes = (size_t)p.pitch * p.h * 4;
        p.ow = 1 + (uint32_t)(next() % 90); p.oh = 1 + (uint32_t)(next() % 70); p.opitch = p.ow + (uint32_t)(next() % 5);
        p.other_bytes = (size_t)p.opitch * p.oh * 4;
        for (int s = 0; s < 2; s++) {
            p.px[s] = guarded(p.fb_bytes, &p.sizes[s], &p.maps[s]);
            p.vga[s] = guarded(80 * 25 * 2, &p.sizes[2 + s], &p.maps[2 + s]);
            p.other[s] = guarded(p.other_bytes, &p.sizes[4 + s], &p.maps[4 + s]);
            uint32_t seed = (uint32_t)sc * 2654435761u;
            for (size_t i = 0; i < p.fb_bytes / 4; i++) p.px[s][i] = seed ^ (uint32_t)i;
            memset(p.vga[s], 0x5a, 80 * 25 * 2);
            for (size_t i = 0; i < p.other_bytes / 4; i++) p.other[s][i] = ~seed ^ (uint32_t)i;
        }
        fb_mem[0] = p.px[0]; fb_mem[1] = p.px[1];
        ref_reset(); cand_reset();
        /* Each side begins with some early output, then one of: real screen_init over a validated BGRX32 tag (or a
         * refused mapping), a directly placed framebuffer console, or a VGA text console. */
        unsigned early = (unsigned)(next() % 40);
        for (unsigned i = 0; i < early; i++) { char c = (char)(next() % 4 ? 0x20 + next() % 95 : '\n'); ref_screen_putc(c); cand_screen_putc(c); }
        unsigned mode = (unsigned)(next() % 6);
        struct boot_info bi; memset(&bi, 0, sizeof bi); bi.efi = 1; bi.fb_tags = 1;
        fb_cfg = (struct boot_fb){BOOT_FB_BGRX32, 0xfd000000ull, (uint64_t)p.pitch * p.h * 4, p.pitch * 4, p.w, p.h};
        fb_err_cfg = NULL; devmap_err_cfg = mode == 5 ? 3 : 0;
        if (mode <= 2 || mode == 5) {
            side_now = 0; ref_screen_init(&bi); side_now = 1; cand_screen_init(&bi);
            if (mode == 5) cov_init_refused++; else cov_init_fb += ref_screen_kind() == SCREEN_FB;
        }
        else if (mode == 3) {
            uint32_t ly = 36, c = (p.w > 32 ? (p.w - 32) / 8 : 8), r = (p.h > 52 ? (p.h - 52) / 18 : 2);
            if (c < 8) c = 8;
            if (r < 2) r = 2;
            ref_set_fb(p.px[0], p.pitch, p.w, p.h, ly, c, r); cand_set_fb(p.px[1], p.pitch, p.w, p.h, ly, c, r);
        } else { ref_set_vga(p.vga[0]); cand_set_vga(p.vga[1]); p.fb_bytes = p.fb_bytes; }
        compare(&p, ops);
        if (big && (next() & 3)) {   /* most desktop-sized scenarios open the scene and a GUI surface early */
            int x = ref_screen_desktop_begin(), y = cand_screen_desktop_begin(); cov_desktop += x == 1; if (x != y) fail("desktop_begin", ops);
            static const uint32_t probe[][2] = {{257, 1}, {1, 161}, {0, 5}, {5, 0}};   /* size edges, all refused */
            for (unsigned i = 0; i < 4; i++) { x = ref_screen_gui_begin(probe[i][0], probe[i][1]); y = cand_screen_gui_begin(probe[i][0], probe[i][1]); if (x != y) fail("gui_begin edge", ops); }
            uint32_t w = (next() & 1) ? 256 : 1 + (uint32_t)(next() % 256), h = (next() & 1) ? 160 : 1 + (uint32_t)(next() % 160);
            x = ref_screen_gui_begin(w, h); y = cand_screen_gui_begin(w, h); cov_gui += x == 1; if (x != y) fail("gui_begin", ops);
            if (next() & 1) { uint32_t u = ref_screen_pointer_begin(), v = cand_screen_pointer_begin(); if (u != v) fail("pointer_begin", ops); }
            compare(&p, ops);
        }
        /* Systematic byte coverage: every byte value once (printable range edges, newline, controls, DEL, high bytes). */
        for (unsigned c = 0; c < 256; c++) { ref_screen_putc((char)c); cand_screen_putc((char)c); }
        compare(&p, ops);
        struct screen_surface other[2] = {{p.other[0], p.opitch, p.ow, p.oh}, {p.other[1], p.opitch, p.ow, p.oh}};
        for (unsigned o = 0; o < per; o++) {
            ops++; uint64_t r = next() % 1000, a = next(), b = next();
            { struct screen_state s; ref_state(&s);
              if (s.gui_active && r < 330 && (a & 1)) r = 650 + a % 85;   /* pointer-heavy while a surface is shown */
              else if (s.cur_drawn && r < 330 && (a & 6) == 2) r = (a & 8) ? 745 + a % 120 : 865 + a % 110; }   /* drawing near the cursor */
            if (r < 330) {   /* console output */
                char c = (char)((a & 7) == 0 ? '\n' : (a & 7) == 1 ? (char)(a >> 8) : (char)(0x20 + (a >> 8) % 95));
                struct screen_state s0, s1; ref_state(&s0); ref_screen_putc(c); cand_screen_putc(c); ref_state(&s1);
                cov_scroll += s0.input_mode && s1.cy == s0.cy && s0.cy + 1 == s0.rows && (c == '\n' || s0.cx >= s0.cols);
                cov_ring_wrap += !s0.input_mode && s1.cy == 0 && s0.cy + 1 == s0.rows;
            } else if (r < 430) {
                struct screen_state s0, s1; ref_state(&s0); ref_screen_erase_previous(); cand_screen_erase_previous(); ref_state(&s1);
                cov_erase_back += s0.cx == 0 && s1.cy != s0.cy;
            } else if (r < 445) { ref_screen_begin_input(); cand_screen_begin_input();
            } else if (r < 455) { ref_screen_restart_log(); cand_screen_restart_log();
            } else if (r < 480) { int st = (int)(a % 5) - 1; const char *t = texts[b % 5]; ref_screen_status(st, t); cand_screen_status(st, t);
            } else if (r < 482) { const char *m = texts[a % 5]; ref_screen_panic(m); cand_screen_panic(m);
            } else if (r < 510) { int x = ref_screen_desktop_begin(), y = cand_screen_desktop_begin(); mix((uint64_t)x); cov_desktop += x == 1; if (x != y) fail("desktop_begin", ops);
            } else if (r < 545) {
                static const uint32_t gw[] = {0, 1, 2, 63, 64, 128, 255, 256, 257, 4096, 0xffffffffu}, gh[] = {0, 1, 2, 80, 159, 160, 161, 4096, 0xffffffffu};
                uint32_t w = (a & 3) ? 1 + (uint32_t)((a >> 8) % 256) : pick(gw, 11), h = (b & 3) ? 1 + (uint32_t)((b >> 8) % 160) : pick(gh, 9);
                int x = ref_screen_gui_available(), y = cand_screen_gui_available(); if (x != y) fail("gui_available", ops);
                x = ref_screen_gui_begin(w, h); y = cand_screen_gui_begin(w, h); mix((uint64_t)x << 1 | (uint64_t)y); cov_gui += x == 1; if (x != y) fail("gui_begin", ops);
            } else if (r < 590) {
                uint32_t row[260], count = (uint32_t)(b % 4 == 0 ? b % 300 : 0), y = (uint32_t)(a % 200);
                struct screen_state s; cand_state(&s); if (!count) count = s.gui_w;
                for (unsigned i = 0; i < 260; i++) row[i] = (uint32_t)(a * 2654435761u) ^ i * 0x01010101u;
                const uint32_t *px = (b & 15) == 1 ? NULL : row; if (count > 260) count = 260;
                ref_screen_gui_row(y, px, count); cand_screen_gui_row(y, px, count);
            } else if (r < 600) { ref_screen_gui_end(); cand_screen_gui_end();
            } else if (r < 625) { uint32_t x = ref_screen_pointer_begin(), y = cand_screen_pointer_begin(); mix(x); if (x != y) fail("pointer_begin", ops);
            } else if (r < 735) {
                static const int32_t big_d[] = {0, 1, -1, 7, -7, 255, -255, 1023, -1023, 0x7fffffff, (int32_t)0x80000000};
                int32_t dx = (a & 3) == 0 ? big_d[(a >> 2) % 11] : (int32_t)((a >> 8) % 61) - 30;
                int32_t dy = (b & 3) == 0 ? big_d[(b >> 2) % 11] : (int32_t)((b >> 8) % 61) - 30;
                struct screen_state g; ref_state(&g);
                if (g.gui_active && g.cur_active && (a & 12) == 4) {   /* aim exactly at a surface or screen boundary */
                    int64_t tx[] = {(int64_t)g.gui_x - 1, g.gui_x, (int64_t)g.gui_x + g.gui_w - 1, (int64_t)g.gui_x + g.gui_w, 0, (int64_t)p.w - 1};
                    int64_t ty[] = {(int64_t)g.gui_y - 1, g.gui_y, (int64_t)g.gui_y + g.gui_h - 1, (int64_t)g.gui_y + g.gui_h, 0, (int64_t)p.h - 1};
                    dx = (int32_t)(tx[(a >> 4) % 6] - g.cur_x); dy = (int32_t)(ty[(b >> 4) % 6] - g.cur_y);
                }
                uint32_t x = ref_screen_pointer_move(dx, dy), y = cand_screen_pointer_move(dx, dy); mix(x); if (x != y) fail("pointer_move", ops);
                struct screen_state s; ref_state(&s); if (s.cur_active) { cov_inside += (x & 0x20000u) != 0; cov_outside += (x & 0x20000u) == 0; }
            } else if (r < 745) { ref_screen_pointer_end(); cand_screen_pointer_end();
            } else if (r < 865) {   /* fills: on the framebuffer (cursor damage applies) or on another surface */
                static const uint32_t ext[] = {0, 1, 7, 8, 9, 31, 32, 33, 100, 0x7fffffffu, 0xfffffffeu, 0xffffffffu};
                int on_fb = (a & 1) && p.fb_bytes;
                uint32_t lw = on_fb ? p.w : p.ow, lh = on_fb ? p.h : p.oh;
                uint32_t x = (a >> 1) % 4 == 0 ? pick(ext, 12) : (uint32_t)((a >> 8) % (lw + 40)), y = (b & 3) == 0 ? pick(ext, 12) : (uint32_t)((b >> 8) % (lh + 40));
                uint32_t w = (b >> 2) % 3 == 0 ? pick(ext, 12) : (uint32_t)((b >> 20) % (lw + 10)), h = (a >> 40) % 3 == 0 ? pick(ext, 12) : (uint32_t)((a >> 44) % (lh + 10));
                uint32_t color = (uint32_t)(a ^ b);
                struct screen_state g; ref_state(&g);
                if (on_fb && g.cur_drawn && (b & 1)) {   /* aim at the cursor canvas boundaries */
                    int64_t xs[] = {(int64_t)g.cur_x - 9, (int64_t)g.cur_x - 1, g.cur_x, (int64_t)g.cur_x + 31, (int64_t)g.cur_x + 32};
                    int64_t ys[] = {(int64_t)g.cur_y - 9, (int64_t)g.cur_y - 1, g.cur_y, (int64_t)g.cur_y + 31, (int64_t)g.cur_y + 32};
                    x = (uint32_t)xs[(a >> 3) % 5]; y = (uint32_t)ys[(a >> 6) % 5]; w = 1 + (uint32_t)((b >> 3) % 9); h = 1 + (uint32_t)((b >> 7) % 9);
                }
                if (on_fb) { ref_screen_fill(ref_fb(), x, y, w, h, color); cand_screen_fill(cand_fb(), x, y, w, h, color); }
                else { ref_screen_fill(&other[0], x, y, w, h, color); cand_screen_fill(&other[1], x, y, w, h, color); }
            } else if (r < 975) {   /* glyphs at and around every edge */
                static const uint32_t ext[] = {0, 1, 7, 8, 9, 15, 16, 17, 0x7fffffffu, 0xfffffff8u, 0xffffffffu};
                int on_fb = (a & 1) && p.fb_bytes;
                uint32_t lw = on_fb ? p.w : p.ow, lh = on_fb ? p.h : p.oh;
                uint32_t x = (a >> 1) % 4 == 0 ? pick(ext, 11) : (uint32_t)((a >> 8) % (lw + 12)), y = (b & 3) == 0 ? pick(ext, 11) : (uint32_t)((b >> 8) % (lh + 20));
                char c = (char)(b >> 32);
                struct screen_state g; ref_state(&g);
                if (on_fb && g.cur_drawn && (b & 1)) {   /* glyph cells touching the cursor canvas edges */
                    int64_t xs[] = {(int64_t)g.cur_x - 8, (int64_t)g.cur_x - 7, (int64_t)g.cur_x + 31, (int64_t)g.cur_x + 32};
                    int64_t ys[] = {(int64_t)g.cur_y - 16, (int64_t)g.cur_y - 15, (int64_t)g.cur_y + 31, (int64_t)g.cur_y + 32};
                    x = (uint32_t)xs[(a >> 3) % 4]; y = (uint32_t)ys[(a >> 6) % 4];
                }
                if (on_fb) { ref_screen_glyph(ref_fb(), x, y, c, 0xc9d1d9u, 0x0e1116u); cand_screen_glyph(cand_fb(), x, y, c, 0xc9d1d9u, 0x0e1116u); }
                else { ref_screen_glyph(&other[0], x, y, c, 0xffffffu, 0); cand_screen_glyph(&other[1], x, y, c, 0xffffffu, 0); }
            } else {   /* move the text position to any place the module itself can reach */
                struct screen_state s; ref_state(&s);
                uint32_t cmax = s.input_mode ? (s.cols < 80 ? s.cols : 80) : s.cols, rmax = s.input_mode ? (s.rows < 25 ? s.rows : 25) : s.rows;
                if (cmax <= 4096 && rmax) { uint32_t x = (uint32_t)(a % (cmax + 1)), y = (uint32_t)(b % rmax); ref_set_pos(x, y); cand_set_pos(x, y); }
            }
            int k1 = ref_screen_kind(), k2 = cand_screen_kind();
            if (k1 != k2 || strcmp(ref_screen_describe(), cand_screen_describe())) fail("kind/describe", ops);
            compare(&p, ops);
        }
        for (int i = 0; i < 6; i++) munmap(p.maps[i], p.sizes[i]);
    }
    printf("SCREEN COVERAGE desktop=%llu gui=%llu inside=%llu outside=%llu drawn=%llu input=%llu vga=%llu scroll=%llu ring_wrap=%llu erase_back=%llu init_fb=%llu init_refused=%llu\n",
           (unsigned long long)cov_desktop, (unsigned long long)cov_gui, (unsigned long long)cov_inside, (unsigned long long)cov_outside,
           (unsigned long long)cov_drawn, (unsigned long long)cov_input, (unsigned long long)cov_vga, (unsigned long long)cov_scroll,
           (unsigned long long)cov_ring_wrap, (unsigned long long)cov_erase_back, (unsigned long long)cov_init_fb, (unsigned long long)cov_init_refused);
    printf("SCREEN DIFFERENTIAL scenarios=%u ops=%llu checks=%llu mismatches=%llu digest=%016llx\n", scenarios,
           (unsigned long long)ops, (unsigned long long)checks, (unsigned long long)mismatches, (unsigned long long)digest);
    return mismatches ? 1 : 0;
}
