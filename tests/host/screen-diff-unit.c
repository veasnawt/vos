/* R3-G2: one side of the screen.c whole-module differential. Compiled twice: once over the frozen accepted
 * tests/reference/screen-r3g2.c (PFX=ref_) and once over the production arch/x86_64/screen.c (PFX=cand_), with every
 * public screen/desktop symbol renamed per side on the command line. The accepted desktop.c renderer is included so each
 * side draws its own scene; desktop_compute()/desktop_requested() are the shared Rust (R3-G1). These helpers only reset,
 * place and read the module's own statics within the invariants the module itself maintains. */
#include <string.h>
#include SCREEN_SRC
#include "../../arch/x86_64/desktop.c"
#include "screen-diff.h"
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define F(n) CAT(PFX, n)

void F(reset)(void)
{
    kind = SCREEN_NONE; inited = busy = panicked = 0; why = "not initialized";
    memset(&fb, 0, sizeof fb); gui_active = 0; gui_x = gui_y = gui_w = gui_h = 0;
    cols = rows = log_y = cx = cy = 0; text_x = SCREEN_MARGIN; clear_x = clear_w = clear_bottom = 0;
    desktop_mode = 0; memset(&desktop, 0, sizeof desktop); vga = 0; status = SCREEN_BOOTING; status_text = 0;
    memset(early, 0, sizeof early); early_len = 0; input_mode = 0; memset(input_cells, 0, sizeof input_cells);
    memset(&cursor, 0, sizeof cursor);
}
void F(set_fb)(uint32_t *base, uint32_t pitch, uint32_t w, uint32_t h, uint32_t ly, uint32_t c, uint32_t r)
{
    inited = 1; kind = SCREEN_FB; fb = (struct screen_surface){base, pitch, w, h};
    clear_w = w; clear_bottom = h; log_y = ly; cols = c; rows = r; cx = cy = 0;
}
void F(set_vga)(uint16_t *cells) { inited = 1; kind = SCREEN_VGA_TEXT; vga = cells; cols = VGA_COLS; rows = VGA_ROWS - 1; cx = cy = 0; }
/* A text position the module itself can reach: cx <= cols, cy < rows (bounded by the 80x25 shadow in input mode). */
void F(set_pos)(uint32_t x, uint32_t y) { cx = x; cy = y; }
const struct screen_surface *F(fb)(void) { return &fb; }
void F(state)(struct screen_state *s)
{
    memset(s, 0, sizeof *s);
    s->kind = kind; s->inited = inited; s->busy = busy; s->panicked = panicked; s->gui_active = gui_active;
    s->desktop_mode = desktop_mode; s->input_mode = input_mode; s->status = status;
    s->gui_x = gui_x; s->gui_y = gui_y; s->gui_w = gui_w; s->gui_h = gui_h; s->cols = cols; s->rows = rows;
    s->log_y = log_y; s->cx = cx; s->cy = cy; s->text_x = text_x; s->clear_x = clear_x; s->clear_w = clear_w;
    s->clear_bottom = clear_bottom; s->cur_active = cursor.active; s->cur_drawn = cursor.drawn; s->cur_x = cursor.x; s->cur_y = cursor.y;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof input_cells; i++) h = (h ^ (uint8_t)input_cells[i]) * 1099511628211ull;
    s->cells_hash = h; h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof cursor.saved; i++) h = (h ^ ((const uint8_t *)cursor.saved)[i]) * 1099511628211ull;
    s->saved_hash = h;
    if (why) strncpy(s->why, why, sizeof s->why - 1);
    s->layout[0] = desktop.x; s->layout[1] = desktop.y; s->layout[2] = desktop.w; s->layout[3] = desktop.h;
    s->layout[4] = desktop.text_x; s->layout[5] = desktop.text_y; s->layout[6] = desktop.text_w; s->layout[7] = desktop.text_h;
}
