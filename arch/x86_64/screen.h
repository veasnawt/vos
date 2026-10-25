/*
 * Early-boot screen console (U1): a mirror of the serial log on whatever
 * display the boot positively provides, plus a status bar (BOOTING, PASS,
 * FAIL) and a panic banner. Serial (console.h) stays the primary log.
 *
 * Selection (screen_init, after devmap_init), in this order:
 *   1. a framebuffer tag boot_framebuffer accepts as 32-bpp BGRX, mapped
 *      through the device window (write-combining when the PAT allows):
 *      a pixel console with the original 8x16 font (font8x16.h);
 *   2. a framebuffer tag that reports 80x25 EGA text at 0xB8000: VGA text;
 *   3. no framebuffer tag at all on a BIOS boot (no EFI system table) whose
 *      BIOS data area says 80-column text mode (mode 3 or 7): VGA text;
 *   4. anything else - UEFI without a usable framebuffer, a refused or
 *      unmappable framebuffer, graphics mode without framebuffer
 *      information - no screen: the log is on serial only.
 * VGA text memory is never written unless (2) or (3) holds; nothing is
 * written to any screen before screen_init (output until then is kept in a
 * small buffer and replayed).
 *
 * Rendering never reads the framebuffer. Lines wrap at the right margin;
 * when the log area is full, output continues at its top, and the line
 * after the newest one is kept blank so the end of the log stays visible.
 * N4's opt-in screen_begin_input switches to an at-most-80x25 chronological
 * text area. Its small character shadow supports scrolling without VRAM reads;
 * normal boot/status/panic rendering retains the original circular log behavior.
 */
#ifndef VOS_X86_64_SCREEN_H
#define VOS_X86_64_SCREEN_H

#include <stdint.h>

#include "bootinfo.h"

#define SCREEN_NONE         0
#define SCREEN_VGA_TEXT     1
#define SCREEN_FB           2

#define SCREEN_BOOTING      0
#define SCREEN_PASS         1
#define SCREEN_FAIL         2

/* Framebuffer layout (pixels). */
#define SCREEN_MARGIN       16
#define SCREEN_BAR_H        28
#define SCREEN_LINE_H       18      /* 16-pixel glyphs and 2 pixels of leading */

void screen_init(const struct boot_info *bi);
/* Mirror one character of the serial log (printable ASCII and '\n'; '\r' and others ignored). */
void screen_putc(char c);
/* Erase the preceding cell, including a soft wrap. Caller bounds the input line. */
void screen_erase_previous(void);
/* Clear the display's text area for input, then scroll in reading order.
 * Serial boot diagnostics remain intact. No-op without an initialized screen.
 */
void screen_begin_input(void);
/* Opt-in N6 scene around the existing exclusive foreground console. Falls back
 * without changing mode when no validated >=640x480 BGRX framebuffer exists. */
int screen_desktop_begin(void);
/* The status shown in the bar; text (may be NULL) is shown beside it. */
void screen_status(int state, const char *text);
/* The panic banner (called by panic). */
void screen_panic(const char *msg);
int screen_kind(void);
/* Why there is no screen, or what the screen is. */
const char *screen_describe(void);

/* A 32-bpp BGRX pixel surface (the framebuffer, or a buffer in the self-tests). */
struct screen_surface {
    volatile uint32_t *base;
    uint32_t pitch_px;          /* pixels per line (pitch / 4) */
    uint32_t width, height;
};
void screen_fill(const struct screen_surface *s, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void screen_glyph(const struct screen_surface *s, uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);

/*
 * The selection rule alone: how many framebuffer tags there were, the
 * boot_framebuffer result (err NULL = valid fb_kind), whether the boot was
 * UEFI, and the BIOS data area's video mode and columns. Returns SCREEN_*;
 * *why says why.
 */
int screen_choose(uint32_t fb_tags, const char *fb_err, int fb_kind, int efi, uint8_t bda_mode, uint16_t bda_cols,
                  const char **why);

/* Kernel-private bounded presentation; never exported as a user pointer. */
int screen_gui_available(void);
int screen_gui_begin(uint32_t,uint32_t);
void screen_gui_row(uint32_t,const uint32_t *,uint32_t);
void screen_gui_end(void);
#endif
