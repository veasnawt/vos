/* One foreground GUI authority, one <=160 KiB surface. Single CPU, kernel IF=0.
 * No process-owned mappings or device memory; every upload copies before commit.
 */
#include "gui.h"
#include "gui_verify.h"
#include "process.h"
#include "thread.h"
#include "exception.h"
#include "syscall.h"
#include "usercopy.h"
#include "user_console.h"
#include "screen.h"
#include "pmm.h"
#include "memlayout.h"
#include "vmm.h"
#include "cpu.h"
#include "lib.h"
#include "panic.h"

static struct {
    struct process *owner;
    uint64_t pid,token,epoch;
    uint32_t rights,width,height;
    unsigned pages,head,count,lost;
    uint64_t frame[GUI_FRAMES],sequence;
    struct vos_gui_event queue[VOS_GUI_QUEUE];
    struct wait_queue events;
} gui;
static uint64_t next_epoch=1,worker;
static struct process *waiting;
#if defined(VOS_GUI_VERIFY) || defined(VOS_GUI_HOST)
static int alloc_budget=-1;
void gui_test_alloc_budget(int n){alloc_budget=n;}
int gui_waiting(const struct process *p){return waiting==p;}
#endif
static int alloc(uint64_t *pa) {
#if defined(VOS_GUI_VERIFY) || defined(VOS_GUI_HOST)
    if(alloc_budget==0)return PMM_ENOMEM;
    if(alloc_budget>0)alloc_budget--;
#endif
    return pmm_alloc_frame(pa);
}
static int stopping;
static struct wait_queue work;
static uint64_t current_root(void) {
#ifdef VOS_GUI_HOST
    return gui_host_root();
#else
    return read_cr3()&PTE_ADDR;
#endif
}
static uint8_t *page(uint64_t pa) {
#ifdef VOS_GUI_HOST
    return gui_host_frame(pa);
#else
    return phys_to_virt(pa);
#endif
}
static void context(void) {
    if(current_root()!=vmm_kernel_root())panic("GUI trusted setup outside kernel CR3");
}
int gui_owned(const struct process *p) {
    return p && gui.owner==p && proc_file_live(p) && gui.pid==p->pid && !p->kill_reason;
}
unsigned gui_frames(void){return gui.pages;}
static void flush(void) {
    gui.head=gui.count=gui.lost=0;
    memset(gui.queue,0,sizeof(gui.queue));
}
static void free_surface(void) {
    for(unsigned i=0;i<gui.pages;i++) {
        if(pmm_free_frame(gui.frame[i])!=PMM_OK)panic("GUI surface frame ownership");
        gui.frame[i]=0;
    }
    gui.pages=0;gui.token=0;gui.width=gui.height=0;flush();
    screen_gui_end();
}
void gui_revoke(struct process *p) {
    if(!p || p!=gui.owner || p->pid!=gui.pid)return;
    user_console_gui_end(p);
    free_surface();gui.owner=0;gui.pid=0;gui.rights=0;
    thread_wake_all(&gui.events);
}
static void pump(void) {
    struct process *p=gui.owner;
    if(!p || !gui_owned(p) || p->state!=PROC_RUNNING)return;
    for(unsigned i=0;i<VOS_GUI_QUEUE*2;i++) {
        char c=user_console_gui_poll(p);
        if(!c)break;
        if(gui.count==VOS_GUI_QUEUE) {
            if(gui.lost!=UINT32_MAX)gui.lost++;
        } else {
            if(gui.sequence==UINT64_MAX)panic("GUI event sequence exhausted");
            unsigned at=(gui.head+gui.count)%VOS_GUI_QUEUE;
            gui.queue[at]=(struct vos_gui_event){VOS_GUI_KEY_TEXT,(uint8_t)c,++gui.sequence};
            gui.count++;
        }
    }
    if(gui.count || gui.lost)thread_wake_all(&gui.events);
}
#ifdef VOS_GUI_HOST
void gui_host_pump(void){pump();}
#endif
static void input_worker(void *unused) {
    (void)unused;
    for(;;) {
        while(!gui.owner && !stopping)thread_block(&work);
        if(stopping)return;
        gui_verify_tick(gui.owner);
        pump();thread_sleep(1); /* Existing polled i8042, scheduler sleeps; no spin loop. */
    }
}
int gui_service_start(void) {
    context();
    if(worker)return -VOS_EAGAIN;
    stopping=0;memset(&work,0,sizeof(work));
    int64_t id=thread_create("gui-input",input_worker,0);
    if(id<=0)return -VOS_ENOMEM;
    worker=(uint64_t)id;return 0;
}
int gui_service_stop(void) {
    context();
    if(gui.owner || gui.pages)return -VOS_EAGAIN;
    if(!worker)return 0;
    stopping=1;thread_wake_all(&work);thread_interrupt(worker);
    while(thread_state(worker)!=THREAD_FREE)thread_yield();
    worker=0;return 0;
}
int gui_grant(struct process *p,uint32_t rights) {
    context();
    if(!worker || gui.owner || !p || !proc_file_live(p) || p->state!=PROC_READY ||
       p->abi!=5 || p->kill_reason || !rights || (rights&~GUI_ALL))return -VOS_EACCES;
    if(!screen_gui_available())return -VOS_EUNAVAIL;
    if(next_epoch==INT64_MAX)return -VOS_ENOMEM;
    int r=user_console_gui_begin(p);if(r)return r;
    /* No inheritance. The trusted launch route explicitly installs this grant. */
    gui.owner=p;gui.pid=p->pid;gui.rights=rights;gui.epoch=next_epoch++;
    gui.sequence=0;flush();thread_wake_all(&work);return 0;
}
static int caller(struct process *p) {
    struct thread *t=thread_current();
    return gui_owned(p) && p->state==PROC_RUNNING && t && t->proc==p &&
           t->id==p->tid && current_root()==p->root;
}
static int64_t create(struct process *p,const struct exc_frame *f) {
    struct vos_gui_create d;
    if(!(gui.rights&GUI_CREATE))return -VOS_EACCES;
    if(f->rsi!=sizeof(d))return -VOS_EINVAL;
    if(copy_from_user(p->root,&d,f->rdi,sizeof(d)))return -VOS_EFAULT;
    if(d.version!=1 || d.size!=sizeof(d) || !d.width || !d.height ||
       d.width>VOS_GUI_MAX_W || d.height>VOS_GUI_MAX_H ||
       d.stride!=d.width*4 || d.format!=VOS_GUI_BGRX32)return -VOS_EINVAL;
    if(gui.pages)return -VOS_EAGAIN;
    unsigned count=(d.width*d.height*4+PAGE_SIZE-1)/PAGE_SIZE;
    unsigned n=0;
    for(;n<count;n++)if(alloc(&gui.frame[n])!=PMM_OK)break;
    if(n!=count) {
        for(unsigned i=0;i<n;i++) {
            if(pmm_free_frame(gui.frame[i])!=PMM_OK)panic("GUI allocation rollback");
            gui.frame[i]=0;
        }
        return -VOS_ENOMEM;
    }
    gui.pages=count;gui.width=d.width;gui.height=d.height;
    if(!screen_gui_begin(d.width,d.height)){free_surface();return -VOS_EUNAVAIL;}
    if(next_epoch==INT64_MAX){free_surface();return -VOS_ENOMEM;}
    gui.token=next_epoch++;
    return (int64_t)gui.token;
}
static int upload(struct process *p,const struct exc_frame *f) {
    if(!(gui.rights&GUI_UPLOAD))return -VOS_EACCES;
    struct vos_gui_upload d;uint8_t data[VOS_GUI_UPLOAD_MAX];
    if(f->rdx!=sizeof(d))return -VOS_EINVAL;
    if(copy_from_user(p->root,&d,f->rsi,sizeof(d)))return -VOS_EFAULT;
    if(!d.width || !d.height || d.x>gui.width || d.width>gui.width-d.x ||
       d.y>gui.height || d.height>gui.height-d.y || d.stride<d.width*4 ||
       d.stride>VOS_GUI_UPLOAD_MAX || d.stride%4)return -VOS_EINVAL;
    uint64_t size=(uint64_t)(d.height-1)*d.stride+(uint64_t)d.width*4;
    if(size!=d.length || size>sizeof(data))return -VOS_EINVAL;
    if(copy_from_user(p->root,data,d.pixels,size))return -VOS_EFAULT;
    for(unsigned y=0;y<d.height;y++)for(unsigned x=0;x<d.width*4;x++) {
        unsigned at=((d.y+y)*gui.width+d.x)*4+x;
        page(gui.frame[at/PAGE_SIZE])[at%PAGE_SIZE]=data[y*d.stride+x];
    }
    return 0;
}
int64_t gui_syscall(struct process *p,const struct exc_frame *f) {
    if(!p || p->abi<5)return -VOS_ENOSYS;
    if(!caller(p))return -VOS_EACCES;
    if(f->rax==VOS_GUI_CREATE)return create(p,f);
    if(f->rax==VOS_GUI_EVENT) {
        if(!(gui.rights&GUI_EVENTS))return -VOS_EACCES;
        if(f->rsi!=sizeof(struct vos_gui_event) || f->rdx>1)return -VOS_EINVAL;
        if(user_writable(p->root,f->rdi,sizeof(struct vos_gui_event)))return -VOS_EFAULT;
        uint64_t epoch=gui.epoch;
        while(!gui.count && !gui.lost) {
            if(f->rdx)return -VOS_EAGAIN;
            waiting=p;thread_block(&gui.events);waiting=0;
            if(!caller(p) || gui.epoch!=epoch)return -VOS_EINTR;
        }
        if(user_writable(p->root,f->rdi,sizeof(struct vos_gui_event)))return -VOS_EFAULT;
        struct vos_gui_event event=gui.lost ?
            (struct vos_gui_event){VOS_GUI_OVERFLOW,gui.lost,gui.sequence}:gui.queue[gui.head];
        if(copy_to_user(p->root,f->rdi,&event,sizeof(event)))return -VOS_EFAULT;
        if(gui.lost)gui.lost=0;
        else {memset(&gui.queue[gui.head],0,sizeof(event));gui.head=(gui.head+1)%VOS_GUI_QUEUE;gui.count--;}
        return sizeof(event);
    }
    if(f->rax!=VOS_GUI_UPLOAD && f->rax!=VOS_GUI_PRESENT && f->rax!=VOS_GUI_RELEASE)return -VOS_ENOSYS;
    if(!gui.pages || f->rdi!=gui.token)return -VOS_EBADHANDLE;
    if(f->rax==VOS_GUI_UPLOAD)return upload(p,f);
    if(f->rsi || f->rdx)return -VOS_EINVAL;
    if(f->rax==VOS_GUI_RELEASE){free_surface();return 0;}
    if(!(gui.rights&GUI_PRESENT))return -VOS_EACCES;
    /* Only bounded kernel-owned pages enter the display sink. */
    for(unsigned y=0;y<gui.height;y++) {
        uint32_t row[VOS_GUI_MAX_W];
        for(unsigned x=0;x<gui.width;x++) {
            unsigned at=(y*gui.width+x)*4;
            memcpy(&row[x],page(gui.frame[at/PAGE_SIZE])+at%PAGE_SIZE,4);
        }
        screen_gui_row(y,row,gui.width);
    }
    return 0;
}
