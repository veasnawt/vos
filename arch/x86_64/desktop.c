#include "desktop.h"
#include "screen.h"
#include "lib.h"
#define WALL 0x101F2Bu
#define PANEL 0x0E1116u
#define EDGE 0x486879u
#define TITLE 0x203B4Bu
#define INK 0xD9E5ECu
#define MUTED 0x9FBAC8u
/* R3-G1: desktop_requested() and desktop_compute() (desktop.h, unchanged ABI) are
 * safe Rust (kernel/rust/gfx/desktop.rs via arch/x86_64/rust/desktop.rs). The
 * accepted C is frozen as tests/reference/desktop-r3g1.c. Rendering stays here. */
/* All horizontal capacities come from the destination element, not labels. */
static void text(const struct screen_surface *s,uint32_t x,uint32_t y,const char *p,uint32_t right,
                 uint32_t fg,uint32_t bg)
{
    uint32_t room=right>x ? (right-x)/DESKTOP_GLYPH_W : 0;
    for(uint32_t i=0;i<room && p[i];i++)screen_glyph(s,x+i*DESKTOP_GLYPH_W,y,p[i],fg,bg);
}
void desktop_bar(const struct screen_surface *s)
{
    const uint32_t pad=16;
    const char *identity="VOS Desktop";
    uint32_t subtitle=pad+((uint32_t)strlen(identity)+2)*DESKTOP_GLYPH_W;
    screen_fill(s,0,0,s->width,SCREEN_BAR_H,TITLE);
    text(s,pad,(SCREEN_BAR_H-16)/2,identity,subtitle-16,INK,TITLE);
    text(s,subtitle,(SCREEN_BAR_H-16)/2,"Native graphics foundation",s->width-pad,MUTED,TITLE);
}
int desktop_paint(const struct screen_surface *s,const struct desktop_layout *layout)
{
    struct desktop_layout expected;
    if(!s || !s->base || !layout || !desktop_compute(s->width,s->height,&expected) ||
       s->pitch_px<s->width || s->pitch_px>BOOT_FB_MAX_PITCH/4 ||
       (uint64_t)s->pitch_px*s->height>BOOT_FB_MAX_BYTES/4 ||
       memcmp(layout,&expected,sizeof(expected)))return 0;
    screen_fill(s,0,0,s->width,s->height,WALL);
    desktop_bar(s);
    screen_fill(s,layout->x,layout->y,layout->w,layout->h,EDGE);
    screen_fill(s,layout->x+1,layout->y+1,layout->w-2,layout->h-2,PANEL);
    screen_fill(s,layout->x+1,layout->y+1,layout->w-2,28,TITLE);
    text(s,layout->x+12,layout->y+7,"Terminal / native Rust vsh",layout->x+layout->w-12,INK,TITLE);
    screen_fill(s,16,s->height-44,s->width-32,28,TITLE);
    text(s,28,s->height-38,"VOS | Foreground console | apps, hello, rixie",s->width-28,INK,TITLE);
    return 1;
}
