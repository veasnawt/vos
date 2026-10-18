/*
 * Early-boot screen console (U1), VOS_TEST only: the selection rule, the
 * renderer on a RAM surface, the font, and the console this boot chose
 * (framebuffer, VGA text or none), with results PASS, FAIL or NOT APPLICABLE.
 */
#include "selftest.h"
#include "bootinfo.h"
#include "console.h"
#include "devmap.h"
#include "font8x16.h"
#include "lib.h"
#include "memlayout.h"
#include "screen.h"
#include "vmm.h"

#define VGA_PROBE_CELL  (24 * 80 + 79)  /* bottom-right text cell */

static int failures, cases, not_applicable;

static void expect(int ok, const char *what)
{
    cases++;
    if (ok)
        return;
    failures++;
    serial_puts("selftest: FAIL: ");
    serial_puts(what);
    serial_puts("\r\n");
}

/* A check that cannot run on this boot: reported, never counted as passed. */
static void not_applicable_here(const char *what, const char *reason)
{
    not_applicable++;
    serial_puts("selftest: NOT APPLICABLE: ");
    serial_puts(what);
    serial_puts(" (");
    serial_puts(reason);
    serial_puts(")\r\n");
}

static void test_choose(void)
{
    const char *why;

    expect(screen_choose(1, NULL, BOOT_FB_BGRX32, 1, 0, 0, &why) == SCREEN_FB &&
           screen_choose(1, NULL, BOOT_FB_BGRX32, 0, 0xFF, 160, &why) == SCREEN_FB,
           "choose: a valid BGRX framebuffer is used on UEFI and on BIOS (VBE)");
    expect(screen_choose(1, NULL, BOOT_FB_EGA_TEXT, 0, 0, 0, &why) == SCREEN_VGA_TEXT,
           "choose: an EGA text framebuffer tag selects VGA text");
    expect(screen_choose(1, "framebuffer is not 32 bits per pixel", BOOT_FB_NONE, 0, 3, 80, &why) == SCREEN_NONE,
           "choose: a refused framebuffer means no screen, even if the BIOS data area says text mode");
    expect(screen_choose(0, "no framebuffer information", BOOT_FB_NONE, 1, 3, 80, &why) == SCREEN_NONE,
           "choose: UEFI without framebuffer information never writes VGA text memory");
    expect(screen_choose(0, "no framebuffer information", BOOT_FB_NONE, 0, 3, 80, &why) == SCREEN_VGA_TEXT &&
           screen_choose(0, "no framebuffer information", BOOT_FB_NONE, 0, 7, 80, &why) == SCREEN_VGA_TEXT,
           "choose: BIOS without framebuffer information, BIOS data area mode 3 or 7 at 80 columns: VGA text");
    expect(screen_choose(0, "no framebuffer information", BOOT_FB_NONE, 0, 0xFF, 160, &why) == SCREEN_NONE &&
           screen_choose(0, "no framebuffer information", BOOT_FB_NONE, 0, 3, 40, &why) == SCREEN_NONE &&
           screen_choose(0, "no framebuffer information", BOOT_FB_NONE, 0, 0x13, 40, &why) == SCREEN_NONE,
           "choose: a graphics mode (0xFF after VBE, 0x13) or 40 columns is not taken for 80x25 text");
}

#define SW 64
#define SH 40
static uint32_t surf[SW * SH];

static void test_render(void)
{
    const struct screen_surface s = { surf, SW, SW, SH };
    const uint32_t fg = 0x00C9D1D9, bg = 0x000E1116, sentinel = 0xDEADBEEF;
    unsigned wrong = 0, outside = 0;

    for (unsigned i = 0; i < SW * SH; i++)
        surf[i] = sentinel;
    screen_glyph(&s, 8, 4, 'A', fg, bg);
    for (unsigned y = 0; y < SH; y++)
        for (unsigned x = 0; x < SW; x++) {
            int in = x >= 8 && x < 16 && y >= 4 && y < 20;

            if (in) {
                int on = font8x16['A' - FONT8X16_FIRST][y - 4] & (0x80u >> (x - 8));
                wrong += surf[y * SW + x] != (on ? fg : bg);
            } else {
                outside += surf[y * SW + x] != sentinel;
            }
        }
    expect(wrong == 0 && outside == 0, "render: a glyph is exactly its font bits in 8x16 pixels, nothing else touched");

    for (unsigned i = 0; i < SW * SH; i++)
        surf[i] = sentinel;
    screen_glyph(&s, SW - 7, 0, 'B', fg, bg);       /* would cross the right edge */
    screen_glyph(&s, 0, SH - 15, 'C', fg, bg);      /* would cross the bottom */
    screen_glyph(&s, 0xFFFFFFF8u, 0, 'D', fg, bg);  /* far out (no wrap-around) */
    screen_fill(&s, SW - 4, SH - 4, 100, 100, fg);  /* clipped to 4x4 */
    unsigned filled = 0, touched = 0;
    for (unsigned i = 0; i < SW * SH; i++) {
        filled += surf[i] == fg;
        touched += surf[i] != sentinel;
    }
    expect(filled == 16 && touched == 16, "render: glyphs that do not fit are not drawn; fills are clipped to the surface");

    screen_glyph(&s, 0, 0, (char)0x01, fg, bg);     /* a control character renders as '?' */
    unsigned q = 0;
    for (unsigned y = 0; y < 16; y++)
        for (unsigned x = 0; x < 8; x++)
            q += surf[y * SW + x] != ((font8x16['?' - FONT8X16_FIRST][y] & (0x80u >> x)) ? fg : bg);
    expect(q == 0, "render: a character outside printable ASCII renders as '?'");
}

static void test_font(void)
{
    unsigned edge = 0, blank = 0, same = 0;

    for (unsigned g = 0; g < FONT8X16_GLYPHS; g++)
        for (unsigned r = 0; r < 16; r++) {
            edge += font8x16[g][r] & 1;             /* column 7 is the gap between glyphs */
            blank += g == 0 && font8x16[g][r];
        }
    for (unsigned a = 1; a < FONT8X16_GLYPHS; a++)
        for (unsigned b = a + 1; b < FONT8X16_GLYPHS; b++)
            same += memcmp(font8x16[a], font8x16[b], 16) == 0;
    expect(edge == 0 && blank == 0 && same == 0,
           "font: 95 glyphs, column 7 always blank, space empty, no two glyphs identical");
}

/* The console this boot selected. */
static void test_active(const struct boot_info *bi)
{
    int k = screen_kind();
    struct boot_fb f;
    const char *err = boot_framebuffer(bi, &f);

    serial_puts("selftest: screen: ");
    serial_puts(k == SCREEN_FB ? "framebuffer" : k == SCREEN_VGA_TEXT ? "VGA text" : "none");
    serial_puts(" (");
    serial_puts(screen_describe());
    serial_puts(")\r\n");
    expect(!(bi->efi && k == SCREEN_VGA_TEXT), "active: a UEFI boot never uses VGA text memory");
    expect(k != SCREEN_FB || (!err && f.kind == BOOT_FB_BGRX32 && devmap_count() >= 1 && devmap_audit() == NULL),
           "active: the framebuffer console stands on a validated tag and an audited device mapping");

    if (k == SCREEN_FB) {
        struct vmm_audit a;
        uint64_t pages = (f.addr + f.size + PAGE_SIZE - 1) / PAGE_SIZE - f.addr / PAGE_SIZE;

        expect(vmm_audit_space(vmm_kernel_root(), 1, &a) == VMM_OK && a.device_leaves >= pages,
               "active framebuffer: every page mapped in the device window, kernel audit clean");
    } else {
        not_applicable_here("framebuffer console checks", screen_describe());
    }

    if (k == SCREEN_VGA_TEXT) {
        /* Formerly an unconditional check (N1): now only where VGA text is known to exist. */
        volatile uint16_t *v = phys_to_virt(VGA_TEXT_PHYS);
        uint16_t old = v[VGA_PROBE_CELL];

        v[VGA_PROBE_CELL] = 0x0756;
        int ok = v[VGA_PROBE_CELL] == 0x0756;
        v[VGA_PROBE_CELL] = old;
        expect(ok, "active VGA text: text memory is writable through the kernel alias");
    } else {
        not_applicable_here("VGA text memory check", k == SCREEN_FB ? "a framebuffer console is active" : screen_describe());
    }
}

int selftest_screen(const struct boot_info *bi)
{
    failures = cases = not_applicable = 0;
    test_choose();
    test_render();
    test_font();
    test_active(bi);
    serial_puts("selftest: screen ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed, ");
    serial_dec(not_applicable);
    serial_puts(" not applicable\r\n");
    return failures;
}
