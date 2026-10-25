/* N6 first scene: kernel-private layout/rendering, no application display API. */
#ifndef VOS_X86_64_DESKTOP_H
#define VOS_X86_64_DESKTOP_H
#include <stdint.h>
struct screen_surface;
#define DESKTOP_GLYPH_W 8u
struct desktop_layout {
    uint32_t x,y,w,h;
    uint32_t text_x,text_y,text_w,text_h;
};
void desktop_bar(const struct screen_surface *s);
int desktop_requested(const char *cmdline);
int desktop_compute(uint32_t width,uint32_t height,struct desktop_layout *out);
/* The producer owns a validated BGRX32 surface covering pitch_px*height pixels.
 * No framebuffer address or drawing authority is exported to userspace. */
int desktop_paint(const struct screen_surface *s,const struct desktop_layout *layout);
#endif
