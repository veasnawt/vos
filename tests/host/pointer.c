/* Actual shared driver, GUI service and cursor sink. Device/scheduler only
 * are modeled; native firmware evidence separately establishes real operation. */
#ifdef POINTER_RENDER
#define main previous_render_main
#include "desktop.c"
#undef main
/* Independent polygon reference; no duplicate of the renderer's row masks. */
static int reference_inside(int x,int y) {
    static const int point[7][2]={{0,0},{0,23},{6,18},{11,27},{15,25},{11,17},{20,17}};
    int in=0;
    for(unsigned i=0,j=6;i<7;j=i++) {
        int ax=point[j][0],ay=point[j][1],bx=point[i][0],by=point[i][1];
        int cross=(bx-ax)*(y-ay)-(x-ax)*(by-ay);
        if(!cross && x>=(ax<bx?ax:bx) && x<=(ax>bx?ax:bx) &&
           y>=(ay<by?ay:by) && y<=(ay>by?ay:by))return 1;
        if((ay>y)!=(by>y))if((by>ay && cross>0) || (by<ay && cross<0))in=!in;
    }
    return in;
}
static void cursor_geometry(const uint32_t *before,uint32_t width,uint32_t height) {
    CHECK(CURSOR_W==32 && CURSOR_H==32 && sizeof(cursor.saved)==4096);
    CHECK(fb.base[(uint64_t)cursor.y*fb.pitch_px+cursor.x]==0x101a2a);
    for(unsigned y=0;y<32 && y<height-cursor.y;y++)
        for(unsigned x=0;x<32 && x<width-cursor.x;x++) {
            int ink=reference_inside((int)x,(int)y);
            int inner=ink && reference_inside((int)x-1,(int)y) && reference_inside((int)x+1,(int)y) &&
                reference_inside((int)x,(int)y-1) && reference_inside((int)x,(int)y+1);
            size_t at=(size_t)(cursor.y+y)*fb.pitch_px+cursor.x+x;
            uint32_t expected=ink?(inner?0xffffffu:0x101a2au):before[at];
            CHECK(fb.base[at]==expected);
        }
}
static void cursor_case(uint32_t width,uint32_t height,unsigned padding) {
    size_t n=(size_t)(width+padding)*height;
    uint32_t *p=malloc((n+32)*4),*copy=malloc(n*4);CHECK(p && copy);
    for(size_t i=0;i<n+32;i++)p[i]=0xabcdef;
    fb=(struct screen_surface){p+16,width+padding,width,height};
    inited=1;kind=SCREEN_FB;busy=panicked=input_mode=desktop_mode=gui_active=0;
    CHECK(screen_desktop_begin());CHECK(screen_gui_begin(256,160));
    memcpy(copy,(void *)fb.base,n*4);
    uint32_t state=screen_pointer_begin();CHECK((state&0x20000)!=0);
    CHECK(cursor.active && cursor.drawn);
    cursor_geometry(copy,width,height);
    uint32_t row[256];for(unsigned x=0;x<256;x++)row[x]=0x174680;
    for(unsigned y=0;y<160;y++)screen_gui_row(y,row,256);
    CHECK(cursor.drawn);
    screen_pointer_end();
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width+padding;x++) {
        uint32_t expected=x>=gui_x && x<gui_x+256 && y>=gui_y && y<gui_y+160?0x174680:copy[(size_t)y*(width+padding)+x];
        CHECK(fb.base[(size_t)y*(width+padding)+x]==expected);
    }
    memcpy(copy,(void *)fb.base,n*4);
    screen_pointer_begin();
    struct screen_surface reference={copy,width+padding,width,height};
    screen_fill(&reference,cursor.x+9,cursor.y+20,12,8,0xfff6de);
    screen_fill(&fb,cursor.x+9,cursor.y+20,12,8,0xfff6de);
    screen_glyph(&reference,cursor.x+8,cursor.y+16,'A',0x123456,0xfafafa);
    screen_glyph(&fb,cursor.x+8,cursor.y+16,'A',0x123456,0xfafafa);
    cursor_geometry(copy,width,height);
    const int32_t delta[]={INT32_MIN,INT32_MAX,0,-256,256};
    for(unsigned i=0;i<5;i++)for(unsigned j=0;j<5;j++) {
        screen_pointer_move(delta[i],delta[j]);
        CHECK(cursor.x<width && cursor.y<height && cursor.drawn);
        cursor_geometry(copy,width,height);
    }
    screen_pointer_end();CHECK(!memcmp(copy,(void *)fb.base,n*4));
    for(unsigned i=0;i<16;i++)CHECK(p[i]==0xabcdef && p[n+16+i]==0xabcdef);
    screen_gui_end();free(copy);free(p);
}
int main(void) {
    CHECK(previous_render_main()==0);
    unsigned max_x=0,max_y=0,count=0;
    for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++)if(reference_inside((int)x,(int)y)){
        if(x>max_x)max_x=x;
        if(y>max_y)max_y=y;
        count++;
    }
    CHECK(max_x==20 && max_y==27 && count>100 && reference_inside(0,0));
    cursor_case(640,480,0);cursor_case(800,600,13);cursor_case(1024,768,17);
    printf("pointer renderer PASS %u assertions; clipping, stride, underlay restoration, surface redraw\n",checks);return 0;
}
#else
#define console_host_in legacy_console_host_in
#define console_host_out legacy_console_host_out
#define main previous_service_main
#include "gui.c"
#undef main
#undef console_host_in
#undef console_host_out
#include "keyboard.h"
static int model,aux_next,mode_next,fail_aux;
static uint8_t fifo[4096],tags[4096];static unsigned put,get;
static void byte(uint8_t b,uint8_t tag) {
    CHECK(put-get<sizeof(fifo));fifo[put%sizeof(fifo)]=b;tags[put++%sizeof(fifo)]=tag;
}
uint8_t console_host_in(uint16_t port) {
    if(!model)return legacy_console_host_in(port);
    if(port==0x64)return get<put?1|tags[get%sizeof(fifo)]:0;
    CHECK(port==0x60 && get<put);return fifo[get++%sizeof(fifo)];
}
void console_host_out(uint16_t port,uint8_t b) {
    if(!model){legacy_console_host_out(port,b);return;}
    if(port==0x64) {
        CHECK(b==0x20 || b==0x60 || b==0xd4 || b==0xad || b==0xa7 || b==0xae || b==0xa8);
        if(b==0x20)byte(0x60,0);
        if(b==0x60)mode_next=1;
        if(b==0xd4)aux_next=1;
        return;
    }
    CHECK(port==0x60);
    if(mode_next){mode_next=0;return;}
    if(aux_next) {
        aux_next=0;
        if(fail_aux==1)return;
        CHECK(b==0xff || b==0xf6 || b==0xe6 || b==0xf4 || b==0xf5);
        byte(fail_aux==2?0xfe:0xfa,0x20);if(b==0xff){byte(fail_aux==3?0xff:0xaa,0x20);byte(fail_aux==4?3:0,0x20);}return;
    }
    CHECK(b==0xf5 || b==0xf0 || b==2 || b==0xf4);byte(0xfa,0);
}
static void packet(int dx,int dy,int down) {
    byte((uint8_t)(8|(dx<0?16:0)|(dy<0?32:0)|down),0x20);
    byte((uint8_t)dx,0x20);byte((uint8_t)dy,0x20);
}
static void device_tests(void) {
    struct pointer_decoder d={0};struct pointer_packet p;
    for(unsigned h=0;h<256;h++)for(unsigned x=0;x<256;x+=17) {
        d=(struct pointer_decoder){0};int r=pointer_decode(&d,(uint8_t)h,&p);
        if(!(h&8)){CHECK(r==-1 && d.used==0);continue;}
        CHECK(r==0);CHECK(pointer_decode(&d,(uint8_t)x,&p)==0);
        r=pointer_decode(&d,255,&p);CHECK(r==((h&0xc0)?-1:1));
        if(r>0)CHECK(p.dx==(int)x-((h&16)?256:0) && p.dy==255-((h&32)?256:0) && p.buttons==(h&1));
    }
    d=(struct pointer_decoder){0};CHECK(pointer_decode(&d,8,&p)==0);
    for(unsigned i=0;i<15;i++)CHECK(!pointer_idle(&d));
    CHECK(pointer_idle(&d) && !d.used);
    model=1;
    const struct keyboard_io io={console_host_in,console_host_out};
    struct keyboard k;CHECK(keyboard_init(&k,&io));CHECK(keyboard_pointer_start(&k));
    packet(-1,255,1);byte(0x1e,0); /* Interleaved AUX/key sources. */
    CHECK(keyboard_poll(&k)==0);CHECK(keyboard_pointer_poll(&k,&p) && p.dx==-1 && p.dy==255 && p.buttons==1);
    CHECK(keyboard_poll(&k)=='a');
    byte(0x38,0x60); /* parity/timeout on AUX must not touch keyboard modifier state. */
    (void)keyboard_poll(&k);CHECK(keyboard_pointer_lost(&k)>0);
    byte(8,0x20);for(unsigned i=0;i<16;i++)(void)keyboard_pointer_poll(&k,&p);
    CHECK(keyboard_pointer_lost(&k)>0);
    keyboard_pointer_stop(&k);CHECK(!k.pointer_ready);
    CHECK(keyboard_suspend(&k));CHECK(keyboard_resume(&k));
    for(fail_aux=1;fail_aux<=4;fail_aux++) {
        CHECK(keyboard_init(&k,&io));CHECK(!keyboard_pointer_start(&k));CHECK(!k.pointer_ready);
    }
    fail_aux=0;CHECK(keyboard_init(&k,&io));CHECK(keyboard_pointer_start(&k));keyboard_pointer_stop(&k);
    CHECK(sizeof(k)<=128);
    printf("pointer driver context=%zu decoder=%zu bytes; fixed bounds\\n",sizeof(k),sizeof(d));
}
int main(void) {
    CHECK(previous_service_main()==0);device_tests();
    root=0x1000;CHECK(gui_service_start()==0);begin(GUI_ALL|GUI_POINTER);
    CHECK(make(256,160)>0);
    struct vos_gui_event *e=(void *)(arena+128);
    CHECK(call(&a,VOS_GUI_POINTER_STATE,1,16,0)==-VOS_EFAULT);
    CHECK(call(&b,VOS_GUI_POINTER_STATE,(uintptr_t)e,16,0)==-VOS_EACCES);
    CHECK(call(&a,VOS_GUI_POINTER_STATE,(uintptr_t)e,15,0)==-VOS_EINVAL);
    packet(0,0,0);packet(1,1,1);packet(0,0,0);gui_host_pump();
    CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)e,16,1)==16 && e->type==3);
    CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)e,16,1)==16 && e->type==4 && (e->code&0x10000));
    CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)e,16,1)==16 && e->type==4 && !(e->code&0x10000));
    for(unsigned i=0;i<40;i++)packet(1,0,i&1);
    for(unsigned i=0;i<4;i++)gui_host_pump();
    CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)e,16,1)==16 && e->type==2 && e->code>0);
    packet(0,0,0);gui_host_pump();
    CHECK(call(&a,VOS_GUI_POINTER_STATE,(uintptr_t)e,16,0)==16 && e->type==5 && !(e->code&0x10000) && e->sequence>16);
    end();begin(GUI_ALL|GUI_POINTER);CHECK(make(256,160)>0);
    packet(-255,0,0);packet(0,0,1);packet(192,0,1);packet(0,0,0);gui_host_pump();
    CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)e,16,1)==16 && e->type==3 && !(e->code&0x10000));
    CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)e,16,1)==-VOS_EAGAIN);
    packet(0,0,1);packet(-255,0,1);packet(0,0,0);gui_host_pump();
    CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)e,16,1)==16 && e->type==4 && (e->code&0x10000));
    CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)e,16,1)==16 && e->type==3 && (e->code&0x10000) && !(e->code&0x20000));
    CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)e,16,1)==16 && e->type==4 && !(e->code&0x30000));
    end();begin(GUI_ALL);CHECK(make(2,2)>0);
    CHECK(call(&a,VOS_GUI_POINTER_STATE,(uintptr_t)e,16,0)==-VOS_EACCES);
    CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)e,16,1)==-VOS_EAGAIN);end();
    root=0x1000;CHECK(gui_service_stop()==0);CHECK(!frames && worker_state==THREAD_FREE);
    printf("pointer service PASS %u assertions; actual decoder/demux/GUI, failure, bounds, grants, transitions, overflow snapshot, cleanup\n",checks);return 0;
}
#endif
