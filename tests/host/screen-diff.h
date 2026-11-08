/* R3-G2 screen.c whole-module differential: the per-side state record (see screen-diff-unit.c). */
#ifndef VOS_TEST_SCREEN_DIFF_H
#define VOS_TEST_SCREEN_DIFF_H
#include <stdint.h>
struct screen_state {
    int kind, inited, busy, panicked, gui_active, desktop_mode, input_mode, status, cur_active, cur_drawn;
    uint32_t gui_x, gui_y, gui_w, gui_h, cols, rows, log_y, cx, cy, text_x, clear_x, clear_w, clear_bottom, cur_x, cur_y;
    uint32_t layout[8];
    uint64_t cells_hash, saved_hash;
    char why[96];
};
#endif
