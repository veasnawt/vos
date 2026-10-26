/* Real GUI service + console/keyboard/editor; only hardware, scheduler, PMM,
 * address-space registration and display device are modeled. Separate renderer
 * build below exercises the actual screen.c sink against guarded RAM.
 */
#ifdef GUI_RENDER_HOST
#define main desktop_baseline_main
#include "desktop.c"
#undef main
static void gui_render(uint32_t width,uint32_t height,unsigned padding) {
    uint32_t pitch=width+padding;size_t count=(size_t)pitch*height;
    uint32_t *memory=malloc((count+32)*4),*before=malloc(count*4);
    CHECK(memory && before);
    for(size_t i=0;i<count+32;i++)memory[i]=0xC0FFEE;
    fb=(struct screen_surface){memory+16,pitch,width,height};
    inited=1;kind=SCREEN_FB;busy=panicked=input_mode=desktop_mode=gui_active=0;
    CHECK(screen_desktop_begin());
    memcpy(before,(void *)fb.base,count*4);
    CHECK(screen_gui_available());
    CHECK(!screen_gui_begin(0,1) && !screen_gui_begin(257,1) && !screen_gui_begin(1,161));
    CHECK(screen_gui_begin(256,160));
    uint32_t row[256];for(unsigned i=0;i<256;i++)row[i]=0xff000000|i;
    for(unsigned y=0;y<160;y++)screen_gui_row(y,row,256);
    screen_gui_row(UINT32_MAX,row,256);screen_gui_row(0,row,255);screen_gui_row(0,0,256);
    screen_putc('X');
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<pitch;x++) {
        if(x>=gui_x && x<gui_x+256 && y>=gui_y && y<gui_y+160)CHECK(fb.base[(size_t)y*pitch+x]==x-gui_x);
        else CHECK(fb.base[(size_t)y*pitch+x]==before[(size_t)y*pitch+x]);
    }
    screen_gui_end();CHECK(screen_gui_available());
    for(unsigned i=0;i<16;i++)CHECK(memory[i]==0xC0FFEE && memory[count+16+i]==0xC0FFEE);
    kind=SCREEN_NONE;CHECK(!screen_gui_available() && !screen_gui_begin(1,1));
    free(before);free(memory);
}
int main(void) {
    CHECK(desktop_baseline_main()==0);
    gui_render(640,480,0);gui_render(800,600,12);gui_render(1024,768,16);
    printf("GUI renderer PASS %u assertions; actual sink, three sizes, stride/padding, chrome isolation\n",checks);
    return 0;
}
#else
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gui.h"
#include "process.h"
#include "thread.h"
#include "exception.h"
#include "syscall.h"
#include "user_console.h"
#include "pmm.h"
void gui_test_alloc_budget(int);
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
static struct process a,b;
static struct thread current;
static uint64_t root=0x1000,pid=10;
static unsigned frames,active,w,h,worker_state=THREAD_FREE,cancel_block;
static uint8_t pool[64][4096],used[64],arena[8192];
static uint32_t display[160][256];
static uint8_t scans[1024];static unsigned qin,qout,mode_write;
static void push(uint8_t x){CHECK(qin-qout<sizeof(scans));scans[qin++%sizeof(scans)]=x;}
uint8_t console_host_in(uint16_t port){
    if(port==0x64)return qin!=qout ? 1:0;
    CHECK(port==0x60 && qin!=qout);return scans[qout++%sizeof(scans)];
}
void console_host_out(uint16_t port,uint8_t x){
    if(port==0x64){if(x==0x20)push(0x60);if(x==0x60)mode_write=1;return;}
    CHECK(port==0x60);if(mode_write){mode_write=0;return;}
    CHECK(x==0xf5 || x==0xf0 || x==2 || x==0xf4);push(0xfa);
}
uint64_t console_host_root(void){return root;}
uint64_t gui_host_root(void){return root;}
uint64_t vmm_kernel_root(void){return 0x1000;}
void *gui_host_frame(uint64_t pa){CHECK(pa && pa/4096<=64 && used[pa/4096-1]);return pool[pa/4096-1];}
int pmm_alloc_frame(uint64_t *pa){
    for(unsigned i=0;i<64;i++)if(!used[i]){used[i]=1;frames++;memset(pool[i],0,4096);*pa=(i+1)*4096;return PMM_OK;}
    return PMM_ENOMEM;
}
int pmm_free_frame(uint64_t pa){CHECK(pa && pa/4096<=64 && used[pa/4096-1]);used[pa/4096-1]=0;frames--;return PMM_OK;}
__attribute__((noreturn)) void panic(const char *s){fprintf(stderr,"PANIC %s\n",s);abort();}
int proc_file_live(const struct process *p){return (p==&a || p==&b) && (p->state==PROC_READY || p->state==PROC_RUNNING);}
struct thread *thread_current(void){return &current;}
int64_t thread_create(const char *name,thread_fn fn,void *p){(void)name;(void)fn;(void)p;if(worker_state!=THREAD_FREE)return -1;worker_state=THREAD_RUNNABLE;return 99;}
int thread_state(uint64_t id){CHECK(id==99);return worker_state;}
int thread_wake_all(struct wait_queue *q){(void)q;return 0;}
int thread_interrupt(uint64_t id){(void)id;return 1;}
void thread_yield(void){worker_state=THREAD_FREE;}
void thread_sleep(uint64_t ticks){CHECK(ticks==1);}
void thread_block(struct wait_queue *q){(void)q;CHECK(cancel_block);gui_revoke(&a);cancel_block=0;}
void exec_cancel_request(struct process *p){(void)p;}
void serial_putc(char c){(void)c;}
void serial_log_putc(char c){(void)c;}
void serial_puts(const char *s){(void)s;}
void serial_dec(uint64_t n){(void)n;}
void console_erase_previous(void){}
void screen_putc(char c){(void)c;}
void screen_begin_input(void){}
void screen_status(int status,const char *s){(void)status;(void)s;}
int screen_gui_available(void){return !active;}
int screen_gui_begin(uint32_t width,uint32_t height){CHECK(width && width<=256 && height && height<=160 && !active);active=1;w=width;h=height;return 1;}
void screen_gui_row(uint32_t y,const uint32_t *p,uint32_t n){CHECK(active && y<h && n==w);memcpy(display[y],p,n*4);}
void screen_gui_end(void){active=0;}
static int32_t pointer_x,pointer_y;
uint32_t screen_pointer_move(int32_t dx,int32_t dy) {
    int64_t x=(int64_t)pointer_x+dx,y=(int64_t)pointer_y+dy;
    pointer_x=x<0?0:x>=w+128?(int32_t)(w+127):(int32_t)x;pointer_y=y<0?0:y>=h+128?(int32_t)(h+127):(int32_t)y;
    int inside=pointer_x>=64 && pointer_y>=64 && pointer_x<64+(int32_t)w && pointer_y<64+(int32_t)h;
    uint32_t lx=pointer_x<64?0:pointer_x>=64+(int32_t)w?w-1:(uint32_t)pointer_x-64;
    uint32_t ly=pointer_y<64?0:pointer_y>=64+(int32_t)h?h-1:(uint32_t)pointer_y-64;
    return lx|(ly<<8)|(inside?0x20000u:0);
}
uint32_t screen_pointer_begin(void){pointer_x=w/2+64;pointer_y=h/2+64;return screen_pointer_move(0,0);}
void screen_pointer_end(void){}
static int inside(uint64_t p,uint64_t n){return p>=(uintptr_t)arena && n<=sizeof(arena) && p-(uintptr_t)arena<=sizeof(arena)-n;}
int user_writable(uint64_t r,uint64_t p,size_t n){return r==current.root && inside(p,n)?0:-VOS_EFAULT;}
int copy_from_user(uint64_t r,void *d,uint64_t p,size_t n){if(user_writable(r,p,n))return -VOS_EFAULT;memcpy(d,(void *)(uintptr_t)p,n);return 0;}
int copy_to_user(uint64_t r,uint64_t p,const void *s,size_t n){if(user_writable(r,p,n))return -VOS_EFAULT;memcpy((void *)(uintptr_t)p,s,n);return 0;}
static void as(struct process *p){current.proc=p;current.id=p->tid;current.root=p->root;root=p->root;}
static int64_t call(struct process *p,uint64_t nr,uint64_t arg,uint64_t arg2,uint64_t arg3) {
    as(p);return gui_syscall(p,&(struct exc_frame){.rax=nr,.rdi=arg,.rsi=arg2,.rdx=arg3});
}
static void begin(uint32_t rights) {
    CHECK(frames==0);root=0x1000;
    a=(struct process){.pid=++pid,.abi=5,.state=PROC_READY,.root=0x2000,.tid=11};
    b=(struct process){.pid=++pid,.abi=5,.state=PROC_READY,.root=0x3000,.tid=12};
    CHECK(gui_grant(&a,rights)==0);
    CHECK(gui_grant(&b,GUI_ALL)==-VOS_EACCES);
    b.abi=4;CHECK(user_console_grant(&b)==-VOS_EACCES);b.abi=5;
    a.state=PROC_RUNNING;b.state=PROC_RUNNING;as(&a);memset(arena,0,sizeof(arena));
}
static void end(void){gui_revoke(&a);CHECK(!gui_owned(&a) && frames==0 && gui_frames()==0 && !active);}
static int64_t make(uint32_t width,uint32_t height){
    struct vos_gui_create *d=(void *)arena;
    *d=(struct vos_gui_create){1,24,width,height,width*4,1};
    return call(&a,VOS_GUI_CREATE,(uintptr_t)d,24,0);
}
int main(void) {
    root=0x1000;CHECK(gui_service_start()==0);CHECK(gui_service_start()==-VOS_EAGAIN);
    for(unsigned version=0;version<5;version++) {
        a=(struct process){.pid=++pid,.abi=version,.state=PROC_READY};
        CHECK(gui_grant(&a,GUI_ALL)==-VOS_EACCES);
    }
    for(unsigned failure=0;failure<40;failure++) {
        begin(GUI_ALL);gui_test_alloc_budget((int)failure);CHECK(make(256,160)==-VOS_ENOMEM);
        CHECK(frames==0 && gui_frames()==0);gui_test_alloc_budget(-1);end();
    }
    begin(GUI_ALL);
    for(uint32_t width=0;width<260;width++)for(uint32_t height=0;height<164;height++) {
        int64_t token=make(width,height);
        if(width && width<=256 && height && height<=160) {
            CHECK(token>0);
            CHECK(call(&b,VOS_GUI_PRESENT,(uint64_t)token,0,0)==-VOS_EACCES);
            CHECK(call(&a,VOS_GUI_RELEASE,(uint64_t)token,0,0)==0);
        } else CHECK(token==-VOS_EINVAL);
        CHECK(frames==0);
    }
    CHECK(make(UINT32_MAX,1)==-VOS_EINVAL);
    CHECK(make(1,UINT32_MAX)==-VOS_EINVAL);
    CHECK(call(&a,VOS_GUI_CREATE,UINT64_MAX,24,0)==-VOS_EFAULT);
    int64_t token=make(256,160);CHECK(token>0 && frames==40);
    CHECK(make(1,1)==-VOS_EAGAIN);
    struct vos_gui_upload *d=(void *)(arena+64);uint8_t *pixels=arena+512;
    memset(pixels,0x35,4096);*d=(struct vos_gui_upload){0,0,256,4,1024,4096,(uintptr_t)pixels};
    CHECK(call(&a,VOS_GUI_UPLOAD,token,(uintptr_t)d,32)==0);
    CHECK(call(&a,VOS_GUI_PRESENT,token,0,0)==0);CHECK(display[0][0]==0x35353535);
    for(unsigned kind=0;kind<7;kind++) {
        struct vos_gui_upload old=*d;
        switch(kind) {
        case 0:d->x=UINT32_MAX;break;case 1:d->y=UINT32_MAX;break;
        case 2:d->width=UINT32_MAX;break;case 3:d->height=UINT32_MAX;break;
        case 4:d->length=4095;break;case 5:d->stride=UINT32_MAX;break;default:d->pixels=1;
        }
        CHECK(call(&a,VOS_GUI_UPLOAD,token,(uintptr_t)d,32)==(kind==6?-VOS_EFAULT:-VOS_EINVAL));
        *d=old;
        CHECK(call(&a,VOS_GUI_PRESENT,token,0,0)==0 && display[0][0]==0x35353535);
    }
    struct vos_gui_event *event=(void *)(arena+128);
    CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)event,16,1)==-VOS_EAGAIN);
    for(unsigned i=0;i<40;i++)push(0x1e);
    as(&a);gui_host_pump();gui_host_pump();
    CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)event,16,1)==16 && event->type==VOS_GUI_OVERFLOW && event->code==24);
    for(unsigned i=0;i<16;i++) {
        CHECK(call(&a,VOS_GUI_EVENT,1,16,1)==-VOS_EFAULT); // bad destination retains event
        CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)event,16,1)==16 && event->type==VOS_GUI_KEY_TEXT && event->code=='a' && event->sequence==i+1);
    }
    CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)event,16,1)==-VOS_EAGAIN);
    cancel_block=1;CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)event,16,0)==-VOS_EINTR);end();
    begin(GUI_EVENTS);CHECK(make(1,1)==-VOS_EACCES);end();
    begin(GUI_ALL);CHECK(call(&a,VOS_GUI_EVENT,(uintptr_t)event,16,1)==-VOS_EAGAIN);end();
    // Text console reacquires the original keyboard; no stale GUI input reaches it.
    root=0x1000;a.state=PROC_READY;a.abi=4;CHECK(user_console_grant(&a)==0);user_console_revoke(&a);
    root=0x1000;CHECK(gui_service_stop()==0);
    CHECK(frames==0 && worker_state==THREAD_FREE);
    printf("GUI service PASS %u assertions; actual GUI+console+keyboard, allocation rollback, ownership, pointers, bounds, overflow/revocation\n",checks);
    return 0;
}
#endif
