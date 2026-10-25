/* Actual desktop/screen code against bounded RAM, never a simulated guest UI. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../arch/x86_64/screen.c"
static unsigned checks;
#define CHECK(x) do{checks++;assert(x);}while(0)
static void exercise(uint32_t width,uint32_t height,uint32_t padding)
{
    uint32_t pitch=width+padding;size_t count=(size_t)pitch*height;
    uint32_t *allocation=malloc((count+32)*4),*snapshot=malloc(count*4);
    CHECK(allocation && snapshot);
    for(size_t i=0;i<count+32;i++)allocation[i]=0xC0FFEEu;
    struct screen_surface surface={allocation+16,pitch,width,height};
    struct desktop_layout layout;
    CHECK(desktop_compute(width,height,&layout));
    CHECK(desktop_paint(&surface,&layout));
    CHECK(surface.base[SCREEN_BAR_H*pitch]==0x101F2Bu);
    CHECK(layout.y==SCREEN_BAR_H+16);
    /* The recovered gap contains wallpaper: no second title strip. */
    for(unsigned y=SCREEN_BAR_H;y<layout.y;y++)for(unsigned x=0;x<width;x++)CHECK(surface.base[(size_t)y*pitch+x]==0x101F2Bu);
    /* Match the entire expected top bar, including the former badge area. */
    struct screen_surface expected={snapshot,pitch,width,height};
    memcpy(snapshot,(void *)surface.base,count*4);
    screen_fill(&expected,0,0,width,SCREEN_BAR_H,0x203B4Bu);
    const char *labels[]={"VOS Desktop","Native graphics foundation","Terminal / native Rust vsh","VOS | Foreground console | apps, hello, rixie"};
    uint32_t xs[]={16,120,layout.x+12,28},ys[]={6,6,layout.y+7,height-38};
    uint32_t rights[]={104,width-16,layout.x+layout.w-12,width-28};
    uint32_t colors[]={0xD9E5ECu,0x9FBAC8u,0xD9E5ECu,0xD9E5ECu};
    for(unsigned n=0;n<4;n++) {
        CHECK(xs[n]+strlen(labels[n])*DESKTOP_GLYPH_W<=rights[n]);
        for(unsigned k=0;labels[n][k];k++)screen_glyph(&expected,xs[n]+k*8,ys[n],labels[n][k],colors[n],0x203B4Bu);
    }
    CHECK(!memcmp(snapshot,(void *)surface.base,count*4));
    for(unsigned i=0;i<16;i++)CHECK(allocation[i]==0xC0FFEEu && allocation[count+16+i]==0xC0FFEEu);
    for(unsigned y=0;y<height;y++)for(unsigned x=width;x<pitch;x++)CHECK(surface.base[(size_t)y*pitch+x]==0xC0FFEEu);
    memcpy(snapshot,(void *)surface.base,count*4);
    struct desktop_layout bad=layout;bad.text_x++;
    CHECK(!desktop_paint(&surface,&bad) && !memcmp(snapshot,(void *)surface.base,count*4));
    struct screen_surface invalid=surface;invalid.pitch_px=width-1;
    CHECK(!desktop_paint(&invalid,&layout) && !memcmp(snapshot,(void *)surface.base,count*4));
    invalid=surface;invalid.pitch_px=UINT32_MAX;
    CHECK(!desktop_paint(&invalid,&layout));
    CHECK(!desktop_paint(0,&layout) && !desktop_paint(&surface,0));
    fb=surface;inited=1;kind=SCREEN_FB;busy=panicked=input_mode=desktop_mode=0;
    cols=(width-32)/8;rows=(height-52)/18;log_y=36;cx=cy=0;text_x=SCREEN_MARGIN;
    CHECK(screen_desktop_begin());
    screen_status(SCREEN_PASS,"native console ready");
    CHECK(!memcmp(snapshot,(void *)surface.base,(size_t)pitch*SCREEN_BAR_H*4));
    CHECK(desktop_mode && input_mode && cols<=80 && rows<=25 && cols>=8 && rows>=2);
    memcpy(snapshot,(void *)surface.base,count*4);
    for(unsigned n=0;n<rows+4;n++){screen_putc('a'+n%26);screen_putc('\n');}
    CHECK(cy==rows-1);
    cx=cols;screen_putc('x');screen_erase_previous();screen_erase_previous();
    CHECK(cy==rows-2 && cx==cols-1);
    screen_begin_input();screen_erase_previous();CHECK(cx==0 && cy==0);
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<pitch;x++) {
        int inside=x>=layout.text_x && x<layout.text_x+layout.text_w &&
                   y>=layout.text_y && y<layout.text_y+layout.text_h;
        if(!inside)CHECK(surface.base[(size_t)y*pitch+x]==snapshot[(size_t)y*pitch+x]);
    }
    for(unsigned i=0;i<16;i++)CHECK(allocation[i]==0xC0FFEEu && allocation[count+16+i]==0xC0FFEEu);
    memcpy(snapshot,(void *)surface.base,count*4);
    screen_fill(&surface,UINT32_MAX,0,UINT32_MAX,UINT32_MAX,0);
    screen_glyph(&surface,width-7,height-15,'X',1,0);
    CHECK(!memcmp(snapshot,(void *)surface.base,count*4));
    panicked=1;CHECK(!screen_desktop_begin() && !memcmp(snapshot,(void *)surface.base,count*4));panicked=0;
    kind=SCREEN_VGA_TEXT;CHECK(!screen_desktop_begin());kind=SCREEN_NONE;CHECK(!screen_desktop_begin());
    free(snapshot);free(allocation);
}
int main(void)
{
    CHECK(!desktop_requested(0));CHECK(!desktop_requested(""));
    CHECK(desktop_requested("vos.apps=1 vos.desktop=1"));
    CHECK(desktop_requested(" \tvos.desktop=1\t "));
    CHECK(!desktop_requested("vos.desktop=10"));CHECK(!desktop_requested("xvos.desktop=1"));
    CHECK(!desktop_requested("vos.desktop=1x"));CHECK(!desktop_requested("vos.desktop=0"));
    struct desktop_layout x={0};CHECK(!desktop_compute(639,480,&x));
    CHECK(!desktop_compute(640,479,&x));CHECK(!desktop_compute(UINT32_MAX,768,&x));
    CHECK(!desktop_compute(640,480,0));
    exercise(640,480,0);exercise(800,600,12);exercise(1024,768,16);
    printf("PASS %u assertions: actual refined renderer/console; single header/full captions/no PASS badge, stride/padding/guards, viewport and fallback\n",checks);
    return 0;
}
