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
    return 0;
}
