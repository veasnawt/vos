/* Exercise actual write-only framebuffer/VGA erasure without adding test hooks. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../arch/x86_64/screen.c"

static uint32_t pixels[160*160], expected[160*160];
static uint16_t cells[VGA_COLS*VGA_ROWS];

int main(void)
{
    inited=1; kind=SCREEN_FB; busy=panicked=0;
    fb=(struct screen_surface){ pixels, 160, 160, 160 };
    log_y=36; cols=16; rows=6; cx=cy=0;
    for (unsigned i=0; i<160*160; i++) pixels[i]=expected[i]=0xDEADBEEF;
    screen_putc('a');
    memcpy(expected, pixels, sizeof(pixels));
    struct screen_surface e = { expected, 160, 160, 160 };
    screen_glyph(&e, SCREEN_MARGIN, log_y+1, ' ', COLOR_FG, COLOR_BG);
    screen_erase_previous();
    assert(cx==0 && cy==0 && !memcmp(pixels, expected, sizeof(pixels)));
    cx=cols; cy=rows-1;
    screen_putc('x'); /* wrap from bottom row into ring row 0 */
    assert(cx==1 && cy==0);
    screen_erase_previous(); assert(cx==0 && cy==0);
    screen_erase_previous(); assert(cx==cols-1 && cy==rows-1);
    memcpy(expected, pixels, sizeof(pixels));
    panicked=1; screen_erase_previous();
    assert(cx==cols-1 && !memcmp(pixels, expected, sizeof(pixels)));
    panicked=0; kind=SCREEN_VGA_TEXT; vga=cells; cols=VGA_COLS; rows=VGA_ROWS-1;
    memset(cells, 0x5A, sizeof(cells)); cx=3; cy=1;
    screen_erase_previous();
    assert(cx==2 && cells[2*VGA_COLS+2]==(VGA_LOG<<8|' '));
    assert(cells[2*VGA_COLS+1]==0x5A5A && cells[2*VGA_COLS+3]==0x5A5A);
    kind=SCREEN_NONE; screen_erase_previous(); assert(cx==2);
    puts("PASS: framebuffer exact erase, wrap/ring boundary, panic/none guards and VGA cell bounds");

    kind=SCREEN_FB; cols=16; rows=6; log_y=36; cx=cy=0;
    screen_begin_input();
    assert(input_mode && cx==0 && cy==0);
    for (unsigned r=0;r<rows;r++)
        for (unsigned c=0;c<cols;c++) assert(input_cells[r*INPUT_COLS+c]==' ');
    screen_erase_previous(); assert(cx==0 && cy==0); /* top boundary never wraps */
    for (unsigned r=0;r<rows;r++) { screen_putc((char)('a'+r)); screen_putc('\n'); }
    assert(cy==rows-1 && cx==0);
    for (unsigned r=0;r+1<rows;r++) assert(input_cells[r*INPUT_COLS]==(char)('b'+r));
    assert(input_cells[(rows-1)*INPUT_COLS]==' ');
    memcpy(expected,pixels,sizeof(pixels));
    for (unsigned r=0;r<rows;r++)
        for (unsigned c=0;c<cols;c++)
            screen_glyph(&e,SCREEN_MARGIN+c*8,log_y+r*SCREEN_LINE_H+1,
                         input_cells[r*INPUT_COLS+c],COLOR_FG,COLOR_BG);
    assert(!memcmp(pixels,expected,sizeof(pixels))); /* shadow agrees with every rendered glyph */
    cx=cols; screen_putc('x'); /* scroll on a soft wrap, then erase across that wrap */
    screen_erase_previous(); screen_erase_previous();
    assert(cx==cols-1 && cy==rows-2 && input_cells[cy*INPUT_COLS+cx]==' ');
    kind=SCREEN_VGA_TEXT; vga=cells; cols=VGA_COLS; rows=VGA_ROWS-1;
    screen_begin_input();
    for (unsigned r=0;r<rows;r++) { screen_putc((char)('A'+r)); screen_putc('\n'); }
    for (unsigned r=0;r+1<rows;r++)
        assert(cells[(r+1)*VGA_COLS]==(VGA_LOG<<8|('B'+r)));
    assert(cells[rows*VGA_COLS]==(VGA_LOG<<8|' '));
    puts("PASS: opt-in clear, chronological scroll, framebuffer/shadow agreement, VGA scroll and wrap erasure");
    return 0;
}
