/* Actual console, keyboard, editor and debug emitter. Only hardware, scheduler
 * and process/usercopy registration are modeled; native tests cover ring 3. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "user_console.h"
#include "process.h"
#include "thread.h"
#include "keyboard.h"
#include "input_console.h"
#include "syscall.h"
#include "exception.h"
static unsigned checks,screen_bytes,serial_bytes,sleeps,cancels;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
static struct process parent,child,other;
static struct thread current;
static uint64_t root=0x1000,next_pid=10;
static uint8_t queue[512];static unsigned qin,qout;
static int mode_write,suppress_ack,revoke_on_sleep;
static char output[129];
static const char text[]="visible";
static void push(uint8_t b){CHECK(qin-qout<sizeof(queue));queue[qin++%sizeof(queue)]=b;}
static void ack(void){if(!suppress_ack)push(0xFA);}
uint8_t console_host_in(uint16_t port){
 if(port==0x64)return qin!=qout ? 1:0;
 CHECK(port==0x60 && qin!=qout);return queue[qout++%sizeof(queue)];
}
void console_host_out(uint16_t port,uint8_t byte){
 if(port==0x64){if(byte==0x20)push(0x60);if(byte==0x60)mode_write=1;return;}
 CHECK(port==0x60);
 if(mode_write){mode_write=0;return;}
 CHECK(byte==0xF5 || byte==0xF0 || byte==0x02 || byte==0xF4);ack();
}
uint64_t console_host_root(void){return root;}
uint64_t vmm_kernel_root(void){return 0x1000;}
__attribute__((noreturn)) void panic(const char *m){fprintf(stderr,"PANIC %s\n",m);abort();}
int proc_file_live(const struct process *p){
 return (p==&parent || p==&child || p==&other) &&
        (p->state==PROC_READY || p->state==PROC_RUNNING);
}
struct thread *thread_current(void){return &current;}
void exec_cancel_request(struct process *p){CHECK(p==&parent);cancels++;}
void thread_sleep(uint64_t ticks){
 CHECK(ticks==1 && ++sleeps<100000);
 if(revoke_on_sleep){revoke_on_sleep=0;user_console_revoke(&child);}
}
void serial_putc(char c){serial_bytes++;screen_bytes++;(void)c;}
void serial_log_putc(char c){serial_bytes++;(void)c;}
void serial_puts(const char *p){while(*p)serial_putc(*p++);}
void serial_dec(uint64_t n){(void)n;}
void console_erase_previous(void){}
void screen_putc(char c){(void)c;screen_bytes++;}
void screen_begin_input(void){}
void screen_status(int state,const char *text_){(void)state;(void)text_;}
int user_writable(uint64_t r,uint64_t p,size_t n){
 return r!=current.root || p!=(uintptr_t)output || n>sizeof(output) ? -VOS_EFAULT:0;
}
int copy_to_user(uint64_t r,uint64_t p,const void *s,size_t n){
 if(user_writable(r,p,n))return -VOS_EFAULT;
 memcpy((void *)(uintptr_t)p,s,n);return 0;
}
int copy_from_user(uint64_t r,void *d,uint64_t p,size_t n){
 if(r!=current.root || p!=(uintptr_t)text || n>sizeof(text))return -VOS_EFAULT;
 memcpy(d,(void *)(uintptr_t)p,n);return 0;
}
static void as(struct process *p){
 current.proc=p;current.id=p->tid;current.root=p->root;root=p->root;
}
static int64_t call(struct process *p,uint64_t nr,uint64_t ptr,uint64_t cap){
 as(p);return user_console_syscall(p,&(struct exc_frame){.rax=nr,.rdi=ptr,.rsi=cap});
}
static void init(void){
 qin=qout=0;suppress_ack=mode_write=0;root=0x1000;
 parent=(struct process){.pid=++next_pid,.abi=4,.state=PROC_READY,.root=0x2000};
 child=(struct process){.pid=++next_pid,.abi=3,.state=PROC_READY,.root=0x3000};
 other=(struct process){.pid=++next_pid,.abi=4,.state=PROC_READY,.root=0x4000};
 CHECK(user_console_grant(&parent)==0);
 parent.state=PROC_RUNNING;parent.tid=11;as(&parent);
}
static void begin(void){
 root=0x1000;CHECK(user_console_begin_exec(&parent,&child)==0);
 CHECK(user_console_exec_active(&parent,&child) && user_console_lease_active());
 CHECK(!user_console_owned(&parent) && user_console_owned(&child));
 CHECK(!user_console_debug_visible(&parent) && user_console_debug_visible(&child));
 CHECK(user_console_grant(&other)==-VOS_EACCES);
 child.state=PROC_RUNNING;child.tid=12;as(&child);
}
static void reap(void){
 child.state=PROC_EXITED;user_console_revoke(&child);
 child.state=PROC_FREE;child.pid=0;root=0x1000;
}
static void finish(int restore){
 CHECK(user_console_finish_exec(&parent,&child,restore)==0);
 CHECK(!user_console_lease_active());
}
static void end(void){user_console_revoke(&parent);CHECK(!user_console_owned(&parent));}
int main(void){
 /* ABI v3 grant still works; v0-v2 cannot bootstrap a console. */
 parent=(struct process){.pid=++next_pid,.state=PROC_READY,.abi=2,.root=0x2000};
 CHECK(user_console_grant(&parent)==-VOS_EACCES);
 parent.abi=3;CHECK(user_console_grant(&parent)==0);end();
 init();
 CHECK(call(&parent,SYS_CONSOLE_WRITE,0,4)==-VOS_EFAULT);
 CHECK(call(&parent,SYS_CONSOLE_WRITE,(uintptr_t)text,257)==-VOS_EINVAL);
 CHECK(call(&parent,SYS_CONSOLE_WRITE,(uintptr_t)text,7)==7);
 CHECK(call(&parent,SYS_CONSOLE_READ,(uintptr_t)output,0)==-VOS_EINVAL);
 CHECK(call(&parent,SYS_CONSOLE_READ,1,129)==-VOS_EFAULT);
 /* A completed line retained on EMSGSIZE must not cross the handoff. */
 push(0x18);push(0x26);push(0x20);push(0x1c); /* old + Enter */
 CHECK(call(&parent,SYS_CONSOLE_READ,(uintptr_t)output,2)==-VOS_EMSGSIZE);
 push(0x2a);push(0x1e); /* queued Shift+A must be discarded; modifier reset */
 begin();
 unsigned displayed=screen_bytes;
 CHECK(call(&parent,SYS_CONSOLE_WRITE,(uintptr_t)text,7)==-VOS_EACCES);
 CHECK(screen_bytes==displayed);
 CHECK(call(&parent,SYS_CONSOLE_READ,(uintptr_t)output,129)==-VOS_EACCES);
 push(0x30);push(0x1c); /* b */
 CHECK(call(&child,SYS_CONSOLE_READ,(uintptr_t)output,129)==1 && !strcmp(output,"b"));
 push(0x2d);push(0x1c); /* unconsumed child input */
 reap();CHECK(user_console_grant(&other)==-VOS_EACCES);finish(1);
 CHECK(user_console_owned(&parent) && !user_console_owned(&child));
 push(0x2e);push(0x1c);
 CHECK(call(&parent,SYS_CONSOLE_READ,(uintptr_t)output,129)==1 && !strcmp(output,"c"));end();
 /* Cancellation of an active reader discards its stack-local partial line. */
 init();begin();push(0x1e);revoke_on_sleep=1;
 CHECK(call(&child,SYS_CONSOLE_READ,(uintptr_t)output,129)==-VOS_EINTR);
 reap();finish(1);CHECK(user_console_owned(&parent));end();
 /* Parent revocation prevents restoration and cancels outstanding execution. */
 init();begin();user_console_revoke(&parent);CHECK(cancels>0);
 reap();finish(1);CHECK(!user_console_owned(&parent));end();
 /* Execution-authority revocation/parent termination is supplied by broker. */
 init();begin();reap();finish(0);CHECK(!user_console_owned(&parent));end();
 init();begin();parent.kill_reason="terminated";reap();finish(1);
 CHECK(!user_console_owned(&parent));end();
 /* A recycled pointer with a different PID cannot regain the old authority. */
 init();begin();parent.pid++;reap();finish(1);
 CHECK(!user_console_owned(&parent));end();
 /* An unrelated READY process cannot steal the lease, even after child exit. */
 init();begin();reap();CHECK(user_console_grant(&other)==-VOS_EACCES);finish(1);end();
 /* Bounded device failures fail closed, with no manufactured owner. */
 init();for(unsigned i=0;i<KEYBOARD_DRAIN_LIMIT;i++)push(0x1e);
 root=0x1000;CHECK(user_console_begin_exec(&parent,&child)==-VOS_EUNAVAIL);
 CHECK(!user_console_lease_active() && !user_console_owned(&parent));end();
 init();suppress_ack=1;root=0x1000;
 CHECK(user_console_begin_exec(&parent,&child)==-VOS_EUNAVAIL);
 CHECK(!user_console_lease_active() && !user_console_owned(&parent));suppress_ack=0;end();
 for(unsigned i=0;i<1000;i++){init();begin();reap();finish(1);CHECK(user_console_owned(&parent));end();}
 printf("console-handoff: %u assertions PASS; actual console+keyboard+editor; bounded modeled hardware/scheduler; transfers=%llu restored=%llu\n",
        checks,(unsigned long long)user_console_lease_stats()->transferred,(unsigned long long)user_console_lease_stats()->restored);
 return 0;
}
