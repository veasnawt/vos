#include "screen.h"
#include "desktop.h"
#include "console.h"
#include "devmap.h"
#include "font8x16.h"
#include "lib.h"
#include "memlayout.h"

/* A restrained palette (0xRRGGBB, stored as BGRX: blue in the low byte). */
#define COLOR_BG        0x0E1116u
#define COLOR_FG        0xC9D1D9u
#define COLOR_DIM       0x6E7681u
#define COLOR_BAR       0x161B22u
#define COLOR_PASS      0x2EA043u
#define COLOR_FAIL      0xDA3633u
#define COLOR_PANIC     0xB62324u
#define COLOR_WHITE     0xFFFFFFu

/* VGA text attributes. */
#define VGA_LOG         0x07        /* light grey on black */
#define VGA_BAR         0x70        /* black on light grey */
#define VGA_PASS        0x2F        /* white on green */
#define VGA_FAIL        0x4F        /* white on red */
#define VGA_COLS        80
#define VGA_ROWS        25

#define BDA_VIDEO_MODE  0x449
#define BDA_COLUMNS     0x44A
#define EARLY_BYTES     4096
#define INPUT_COLS      80
#define INPUT_ROWS      25

static int kind = SCREEN_NONE, inited, busy, panicked;
static const char *why = "not initialized";
static struct screen_surface fb;
static int gui_active;
static uint32_t gui_x,gui_y,gui_w,gui_h;
static uint32_t cols, rows, log_y, cx, cy;
static uint32_t text_x=SCREEN_MARGIN,clear_x,clear_w,clear_bottom;
static int desktop_mode;
static struct desktop_layout desktop;
static volatile uint16_t *vga;
static int status = SCREEN_BOOTING;
static const char *status_text;
static char early[EARLY_BYTES];
static uint32_t early_len;
static int input_mode;
static char input_cells[INPUT_COLS * INPUT_ROWS];

int screen_kind(void)
{
    return kind;
}

const char *screen_describe(void)
{
    return why;
}

int screen_choose(uint32_t fb_tags, const char *fb_err, int fb_kind, int efi, uint8_t bda_mode, uint16_t bda_cols,
                  const char **out)
{
    if (!fb_err && fb_kind == BOOT_FB_BGRX32) {
        *out = "framebuffer";
        return SCREEN_FB;
    }
    if (!fb_err && fb_kind == BOOT_FB_EGA_TEXT) {
        *out = "VGA text (the loader reports 80x25 EGA text)";
        return SCREEN_VGA_TEXT;
    }
    if (fb_tags) {                      /* refused: a graphics mode may be active, so no guessing */
        *out = fb_err ? fb_err : "unusable framebuffer information";
        return SCREEN_NONE;
    }
    if (efi) {
        *out = "UEFI boot without framebuffer information";
        return SCREEN_NONE;
    }
    if ((bda_mode == 3 || bda_mode == 7) && bda_cols == VGA_COLS) {
        *out = "VGA text (BIOS data area: 80-column text mode)";
        return SCREEN_VGA_TEXT;
    }
    *out = "no framebuffer information and the BIOS reports no text mode";
    return SCREEN_NONE;
}


/* Fixed 32x32 kernel-owned cursor canvas; visible arrow is 21x28.
 * Hotspot (0,0) is the exact tip. No framebuffer/underlay pointer reaches
 * userspace. Row masks avoid per-motion rasterization or allocation. */
enum { CURSOR_W=32, CURSOR_H=32 };
static const uint32_t cursor_shape[CURSOR_H]={
    0x00000001u,0x00000003u,0x00000007u,0x0000000fu,
    0x0000001fu,0x0000003fu,0x000000ffu,0x000001ffu,
    0x000003ffu,0x000007ffu,0x00000fffu,0x00001fffu,
    0x00007fffu,0x0000ffffu,0x0001ffffu,0x0003ffffu,
    0x0007ffffu,0x001fffffu,0x00000fffu,0x00001f9fu,
    0x00001f0fu,0x00003f07u,0x00003e03u,0x00007e01u,
    0x00007c00u,0x0000fc00u,0x00003800u,0x00000800u,
    0x00000000u,0x00000000u,0x00000000u,0x00000000u
};
static const uint32_t cursor_fill[CURSOR_H]={
    0x00000000u,0x00000000u,0x00000002u,0x00000006u,
    0x0000000eu,0x0000001eu,0x0000003eu,0x000000feu,
    0x000001feu,0x000003feu,0x000007feu,0x00000ffeu,
    0x00001ffeu,0x00007ffeu,0x0000fffeu,0x0001fffeu,
    0x0003fffeu,0x00000ffeu,0x0000079eu,0x00000f0eu,
    0x00000e06u,0x00001e02u,0x00001c00u,0x00003c00u,
    0x00003800u,0x00003800u,0x00000000u,0x00000000u,
    0x00000000u,0x00000000u,0x00000000u,0x00000000u
};
static struct {int active,drawn;uint32_t x,y,saved[CURSOR_W*CURSOR_H];} cursor;
static void cursor_hide(void) {
    if(!cursor.drawn)return;
    for(unsigned y=0;y<CURSOR_H && y<fb.height-cursor.y;y++)
        for(unsigned x=0;x<CURSOR_W && x<fb.width-cursor.x;x++)
            fb.base[(uint64_t)(cursor.y+y)*fb.pitch_px+cursor.x+x]=cursor.saved[y*CURSOR_W+x];
    cursor.drawn=0;
}
static void cursor_show(void) {
    if(!cursor.active || cursor.drawn || panicked)return;
    for(unsigned y=0;y<CURSOR_H && y<fb.height-cursor.y;y++)
        for(unsigned x=0;x<CURSOR_W && x<fb.width-cursor.x;x++) {
            volatile uint32_t *p=fb.base+(uint64_t)(cursor.y+y)*fb.pitch_px+cursor.x+x;
            cursor.saved[y*CURSOR_W+x]=*p;
            uint32_t bit=1u<<x; /* x<32, including clipped right-edge tiles. */
            if(cursor_shape[y]&bit)*p=(cursor_fill[y]&bit)?0x00ffffffu:0x00101a2au;
        }
    cursor.drawn=1;
}
static int cursor_damage(const struct screen_surface *s,uint32_t x,uint32_t y,uint32_t w,uint32_t h) {
    int hit=cursor.drawn && s==&fb && (uint64_t)x<(uint64_t)cursor.x+CURSOR_W && (uint64_t)x+w>cursor.x &&
        (uint64_t)y<(uint64_t)cursor.y+CURSOR_H && (uint64_t)y+h>cursor.y;
    if(hit)cursor_hide();
    return hit;
}
/* ---- bounded pixels ---- */

void screen_fill(const struct screen_surface *s, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    if (x >= s->width || y >= s->height)
        return;
    if (w > s->width - x)
        w = s->width - x;
    if (h > s->height - y)
        h = s->height - y;
    int damaged=cursor_damage(s,x,y,w,h);
    for (uint32_t r = 0; r < h; r++) {
        volatile uint32_t *p = s->base + (uint64_t)(y + r) * s->pitch_px + x;

        for (uint32_t c = 0; c < w; c++)
            p[c] = color;
    }
    if(damaged)cursor_show();
}

void screen_glyph(const struct screen_surface *s, uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
    unsigned i = (unsigned char)c >= FONT8X16_FIRST && (unsigned char)c < FONT8X16_FIRST + FONT8X16_GLYPHS ?
                 (unsigned char)c - FONT8X16_FIRST : '?' - FONT8X16_FIRST;

    if (x > s->width || s->width - x < 8 || y > s->height || s->height - y < 16)
        return;
    int damaged=cursor_damage(s,x,y,8,16);
    for (uint32_t r = 0; r < 16; r++) {
        volatile uint32_t *p = s->base + (uint64_t)(y + r) * s->pitch_px + x;
        uint8_t bits = font8x16[i][r];

        for (uint32_t col = 0; col < 8; col++)
            p[col] = bits & (0x80u >> col) ? fg : bg;
    }
    if(damaged)cursor_show();
}

static void fb_text(uint32_t x, uint32_t y, const char *t, uint32_t max_chars, uint32_t fg, uint32_t bg)
{
    for (uint32_t n = 0; t && *t && n < max_chars; t++, n++)
        screen_glyph(&fb, x + n * 8, y, *t, fg, bg);
}

/* ---- the status bar ---- */

static void draw_bar(void)
{
    /* Ordinary desktop chrome carries identity/context, not a permanent test badge.
     * Failure and panic paths retain their diagnostic display behavior. */
    if(desktop_mode && kind==SCREEN_FB && status!=SCREEN_FAIL) {
        desktop_bar(&fb);
        return;
    }
    static const char *const label[] = { "BOOTING", "PASS", "FAIL" };
    const char *l = label[status];
    uint32_t llen = (uint32_t)strlen(l);

    if (kind == SCREEN_FB) {
        uint32_t badge_w = llen * 8 + 16, badge_x = fb.width - SCREEN_MARGIN - badge_w, ty = (SCREEN_BAR_H - 16) / 2;
        uint32_t badge_c = status == SCREEN_PASS ? COLOR_PASS : status == SCREEN_FAIL ? COLOR_FAIL : COLOR_BAR;
        uint32_t room = badge_x > SCREEN_MARGIN + 13 * 8 ? (badge_x - SCREEN_MARGIN - 13 * 8) / 8 : 0;

        screen_fill(&fb, 0, 0, fb.width, SCREEN_BAR_H, COLOR_BAR);
        fb_text(SCREEN_MARGIN, ty, "VOS", 3, COLOR_WHITE, COLOR_BAR);
        fb_text(SCREEN_MARGIN + 5 * 8, ty, desktop_mode ? "desktop" : input_mode ? "console" : "early boot", 10, COLOR_DIM, COLOR_BAR);
        if (status_text && room > 2)
            fb_text(SCREEN_MARGIN + 17 * 8, ty, status_text, room - 4, COLOR_FG, COLOR_BAR);
        screen_fill(&fb, badge_x, 4, badge_w, SCREEN_BAR_H - 8, badge_c);
        fb_text(badge_x + 8, ty, l, llen, status == SCREEN_BOOTING ? COLOR_DIM : COLOR_WHITE, badge_c);
    } else if (kind == SCREEN_VGA_TEXT) {
        const char *head = input_mode ? " VOS console" : " VOS early boot";
        uint8_t a = status == SCREEN_PASS ? VGA_PASS : status == SCREEN_FAIL ? VGA_FAIL : VGA_BAR;
        unsigned i = 0;

        for (; i < VGA_COLS; i++)
            vga[i] = (uint16_t)(a << 8 | ' ');
        for (i = 0; head[i]; i++)
            vga[i] = (uint16_t)(a << 8 | (uint8_t)head[i]);
        for (unsigned j = 0; j < llen; j++)
            vga[VGA_COLS - 1 - llen + j] = (uint16_t)(a << 8 | (uint8_t)l[j]);
        for (unsigned j = 0; status_text && status_text[j] && i + 2 + j < VGA_COLS - 2 - llen; j++)
            vga[i + 2 + j] = (uint16_t)(a << 8 | (uint8_t)status_text[j]);
    }
}

/* ---- the log area ---- */

static void clear_line(uint32_t r)
{
    if (input_mode)
        for (unsigned i = 0; i < cols; i++)
            input_cells[r * INPUT_COLS + i] = ' ';
    if (kind == SCREEN_FB)
        screen_fill(&fb, desktop_mode ? clear_x : 0, log_y + r * SCREEN_LINE_H,
                    desktop_mode ? clear_w : fb.width, SCREEN_LINE_H, COLOR_BG);
    else
        for (unsigned i = 0; i < VGA_COLS; i++)
            vga[(1 + r) * VGA_COLS + i] = (uint16_t)(VGA_LOG << 8 | ' ');
}

static void newline(void)
{
    cx = 0;
    if (input_mode) {
        if (cy + 1 < rows) {
            clear_line(++cy);
            return;
        }
        /* Shift the character shadow, then redraw. Never read device memory. */
        for (unsigned r = 0; r + 1 < rows; r++)
            for (unsigned c = 0; c < cols; c++)
                input_cells[r * INPUT_COLS + c] = input_cells[(r + 1) * INPUT_COLS + c];
        clear_line(rows - 1);
        for (unsigned r = 0; r + 1 < rows; r++)
            for (unsigned c = 0; c < cols; c++) {
                char ch = input_cells[r * INPUT_COLS + c];
                if (kind == SCREEN_FB)
                    screen_glyph(&fb, text_x + c * 8, log_y + r * SCREEN_LINE_H + 1,
                                 ch, COLOR_FG, COLOR_BG);
                else
                    vga[(1 + r) * VGA_COLS + c] = (uint16_t)(VGA_LOG << 8 | (uint8_t)ch);
            }
        return;
    }
    if (++cy >= rows)
        cy = 0;
    clear_line(cy);
    clear_line((cy + 1) % rows);        /* the gap after the newest line */
}

static void put(char c)
{
    if (c == '\n') {
        newline();
        return;
    }
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E)
        return;
    if (cx >= cols)
        newline();
    if (input_mode)
        input_cells[cy * INPUT_COLS + cx] = c;
    if (kind == SCREEN_FB)
        screen_glyph(&fb, text_x + cx * 8, log_y + cy * SCREEN_LINE_H + 1, c, COLOR_FG, COLOR_BG);
    else
        vga[(1 + cy) * VGA_COLS + cx] = (uint16_t)(VGA_LOG << 8 | (uint8_t)c);
    cx++;
}

void screen_putc(char c)
{
    if(gui_active)return; /* Private GUI content; diagnostics still reach serial. */
    if (!inited) {
        if (early_len < EARLY_BYTES)
            early[early_len++] = c;
        return;
    }
    if (kind == SCREEN_NONE || busy || panicked)
        return;
    busy = 1;
    put(c);
    busy = 0;
}

void screen_erase_previous(void)
{
    if (!inited || kind == SCREEN_NONE || busy || panicked)
        return;
    busy = 1;
    if (!cx) {
        if (input_mode && !cy) {
            busy = 0;
            return;
        }
        cy = (cy + rows - 1) % rows;
        cx = cols;
    }
    cx--;
    if (input_mode)
        input_cells[cy * INPUT_COLS + cx] = ' ';
    if (kind == SCREEN_FB)
        screen_glyph(&fb, text_x + cx * 8, log_y + cy * SCREEN_LINE_H + 1,
                     ' ', COLOR_FG, COLOR_BG);
    else
        vga[(1 + cy) * VGA_COLS + cx] = (uint16_t)(VGA_LOG << 8 | ' ');
    busy = 0;
}

void screen_begin_input(void)
{
    if (!inited || kind == SCREEN_NONE || busy || panicked)
        return;
    busy = 1;
    input_mode = 1;
    if (cols > INPUT_COLS)
        cols = INPUT_COLS;
    if (rows > INPUT_ROWS)
        rows = INPUT_ROWS;
    cx = cy = 0;
    if (kind == SCREEN_FB)
        screen_fill(&fb, desktop_mode ? clear_x : 0, log_y, desktop_mode ? clear_w : fb.width,
                    (desktop_mode ? clear_bottom : fb.height) - log_y, COLOR_BG);
    for (unsigned r = 0; r < rows; r++)
        clear_line(r);
    draw_bar();
    busy = 0;
}

void screen_status(int state, const char *text)
{
    if (state < SCREEN_BOOTING || state > SCREEN_FAIL)
        return;
    status = state;
    status_text = text;
    if (kind == SCREEN_NONE || busy || panicked)
        return;
    busy = 1;
    draw_bar();
    busy = 0;
}

void screen_panic(const char *msg)
{
    if (kind == SCREEN_NONE || busy || panicked)
        return;                         /* a fault while drawing: the serial log has the message */
    busy = 1;
    panicked = 1;
    if (kind == SCREEN_FB) {
        uint32_t ty = (SCREEN_BAR_H - 16) / 2, room = (fb.width - 2 * SCREEN_MARGIN) / 8;

        screen_fill(&fb, 0, 0, fb.width, SCREEN_BAR_H, COLOR_PANIC);
        fb_text(SCREEN_MARGIN, ty, "PANIC:", room, COLOR_WHITE, COLOR_PANIC);
        if (room > 7)
            fb_text(SCREEN_MARGIN + 7 * 8, ty, msg, room - 7, COLOR_WHITE, COLOR_PANIC);
    } else {
        static const char head[] = " PANIC: ";
        unsigned i = 0;

        for (; i < VGA_COLS; i++)
            vga[i] = (uint16_t)(VGA_FAIL << 8 | ' ');
        for (i = 0; head[i]; i++)
            vga[i] = (uint16_t)(VGA_FAIL << 8 | (uint8_t)head[i]);
        for (unsigned j = 0; msg && msg[j] && i < VGA_COLS; j++, i++)
            vga[i] = (uint16_t)(VGA_FAIL << 8 | (uint8_t)((unsigned char)msg[j] >= 0x20 && msg[j] < 0x7F ? msg[j] : '?'));
    }
    busy = 0;
}

/* ---- selection ---- */

void screen_init(const struct boot_info *bi)
{
    struct boot_fb f;
    const char *err = boot_framebuffer(bi, &f);
    uint8_t mode = 0;
    uint16_t bcols = 0;

    if (inited)
        return;
    if (!bi->efi) {                     /* the BIOS data area exists only on BIOS boots */
        mode = *(volatile const uint8_t *)phys_to_virt(BDA_VIDEO_MODE);
        bcols = *(volatile const uint16_t *)phys_to_virt(BDA_COLUMNS);
    }
    kind = screen_choose(bi->fb_tags, err, f.kind, bi->efi, mode, bcols, &why);

    if (kind == SCREEN_FB) {
        void *va;
        int used, derr = devmap_map(f.addr, f.size, DEVMAP_WC, "framebuffer", &va, &used);

        if (derr != DEVMAP_OK) {
            kind = SCREEN_NONE;
            why = devmap_strerror(derr);
            serial_puts("screen: framebuffer at ");
            serial_hex(f.addr);
            serial_puts(" not mapped: ");
            serial_puts(why);
            serial_puts("\r\n");
        } else {
            fb.base = va;
            fb.pitch_px = f.pitch / 4;
            fb.width = f.width;
            fb.height = f.height;
            clear_w=fb.width;clear_bottom=fb.height;
            log_y = SCREEN_BAR_H + 8;
            cols = (fb.width - 2 * SCREEN_MARGIN) / 8;
            rows = (fb.height - log_y - SCREEN_MARGIN) / SCREEN_LINE_H;
            screen_fill(&fb, 0, 0, fb.width, fb.height, COLOR_BG);
            serial_puts("screen: framebuffer ");
            serial_dec(f.width);
            serial_puts("x");
            serial_dec(f.height);
            serial_puts(" BGRX at ");
            serial_hex(f.addr);
            serial_puts(", pitch ");
            serial_dec(f.pitch);
            serial_puts(used == DEVMAP_WC ? ", write-combining" : ", uncached");
            serial_puts(", MTRR type ");
            serial_dec((uint64_t)(devmap_mtrr_type(f.addr) & 0xFF));
            serial_puts(", ");
            serial_dec(cols);
            serial_puts("x");
            serial_dec(rows);
            serial_puts(" cells\r\n");
        }
    } else if (kind == SCREEN_VGA_TEXT) {
        vga = phys_to_virt(VGA_TEXT_PHYS);
        cols = VGA_COLS;
        rows = VGA_ROWS - 1;
        for (unsigned i = 0; i < VGA_COLS * VGA_ROWS; i++)
            vga[i] = (uint16_t)(VGA_LOG << 8 | ' ');
        serial_puts("screen: ");
        serial_puts(why);
        serial_puts("\r\n");
    } else {
        serial_puts("screen: none, serial only: ");
        serial_puts(why);
        serial_puts("\r\n");
    }

    inited = 1;
    if (kind == SCREEN_NONE)
        return;
    if (rows < 2 || cols < 8) {
        kind = SCREEN_NONE;
        why = "screen too small for the console";
        return;
    }
    cx = cy = 0;
    draw_bar();
    for (uint32_t i = 0; i < early_len; i++)   /* the log so far */
        screen_putc(early[i]);
}

int screen_desktop_begin(void)
{
    if(!inited || kind!=SCREEN_FB || busy || panicked ||
       !desktop_compute(fb.width,fb.height,&desktop))return 0;
    busy=1;
    if(!desktop_paint(&fb,&desktop)){busy=0;return 0;}
    desktop_mode=1;text_x=desktop.text_x;clear_x=desktop.text_x;
    log_y=desktop.text_y;clear_w=desktop.text_w;clear_bottom=log_y+desktop.text_h;
    cols=desktop.text_w/8;rows=desktop.text_h/SCREEN_LINE_H;
    busy=0;screen_begin_input();
    return 1;
}

/* Private presentation sink; only the validated kernel GUI service calls it. */
int screen_gui_available(void)
{
    return inited && kind==SCREEN_FB && desktop_mode && !panicked &&
           !busy && !gui_active && clear_w>=256 && clear_bottom>=log_y+160;
}
int screen_gui_begin(uint32_t w,uint32_t h)
{
    if(!screen_gui_available() || !w || !h || w>256 || h>160)return 0;
    gui_w=w;gui_h=h;gui_x=clear_x+(clear_w-w)/2;
    gui_y=log_y+(clear_bottom-log_y-h)/2;gui_active=1;
    screen_fill(&fb,gui_x,gui_y,w,h,0);
    return 1;
}
void screen_gui_row(uint32_t y,const uint32_t *pixels,uint32_t count)
{
    if(!gui_active || panicked || busy || y>=gui_h || count!=gui_w || !pixels)return;
    int damaged=cursor_damage(&fb,gui_x,gui_y+y,count,1);
    volatile uint32_t *row=fb.base+(uint64_t)(gui_y+y)*fb.pitch_px+gui_x;
    for(uint32_t x=0;x<count;x++)row[x]=pixels[x]&0x00ffffffu;
    if(damaged)cursor_show();
}
void screen_gui_end(void)
{
    if(!gui_active)return;
    screen_pointer_end();gui_active=0;
    (void)screen_desktop_begin(); /* Restore the existing scene and console shadow. */
}

uint32_t screen_pointer_move(int32_t dx,int32_t dy) {
    if(!cursor.active || !gui_active)return 0;
    cursor_hide();
    int64_t x=(int64_t)cursor.x+dx,y=(int64_t)cursor.y+dy;
    cursor.x=x<0?0:x>=fb.width?fb.width-1:(uint32_t)x;
    cursor.y=y<0?0:y>=fb.height?fb.height-1:(uint32_t)y;
    cursor_show();
    int inside=cursor.x>=gui_x && cursor.x-gui_x<gui_w &&
               cursor.y>=gui_y && cursor.y-gui_y<gui_h;
    uint32_t lx=cursor.x<gui_x?0:cursor.x-gui_x>=gui_w?gui_w-1:cursor.x-gui_x;
    uint32_t ly=cursor.y<gui_y?0:cursor.y-gui_y>=gui_h?gui_h-1:cursor.y-gui_y;
    return lx|(ly<<8)|(inside?0x20000u:0);
}
uint32_t screen_pointer_begin(void) {
    if(!gui_active || cursor.active)return 0;
    cursor.x=gui_x+gui_w/2;cursor.y=gui_y+gui_h/2;cursor.active=1;
    return screen_pointer_move(0,0);
}
void screen_pointer_end(void) {cursor_hide();cursor.active=0;}
