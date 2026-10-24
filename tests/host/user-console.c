#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../arch/x86_64/user_console.c"

static struct process app;
static struct thread task;
static unsigned char memory[512];
static int reader_calls, alter_pointer, interrupt_read;
static struct exc_frame *live;
static char display[1024], logtext[1024];
static unsigned nd,nl,clears;
int proc_file_live(const struct process *p) { return p==&app && p->pid && (p->state==PROC_READY || p->state==PROC_RUNNING); }
struct thread *thread_current(void) { return &task; }
int keyboard_init(struct keyboard *k,const struct keyboard_io *io) { assert(io); k->ready=1;return 1; }
void screen_begin_input(void) {clears++;}
void screen_status(int state,const char *text) {(void)state;(void)text;}
void screen_putc(char c) { assert(nd<sizeof(display));display[nd++]=c; }
void serial_log_putc(char c) {assert(nl<sizeof(logtext));logtext[nl++]=c;}
static int valid(uint64_t p,size_t n) { return !n || (p>=(uint64_t)memory && p<=(uint64_t)memory+sizeof(memory) && n<=(uint64_t)memory+sizeof(memory)-p); }
int user_writable(uint64_t root,uint64_t p,size_t n) {(void)root;return valid(p,n)?0:-3;}
int copy_from_user(uint64_t root,void *out,uint64_t p,size_t n) {(void)root;if(!valid(p,n))return -3;if(n)memcpy(out,(void*)p,n);return 0;}
int copy_to_user(uint64_t root,uint64_t p,const void *in,size_t n) {(void)root;if(!valid(p,n))return -3;if(n)memcpy((void*)p,in,n);return 0;}
int input_console_read_line_for(struct keyboard *k,char *out,unsigned cap,const char *prompt,int(*cancel)(void*),void*ctx)
{
    assert(k->ready && cap==129 && !*prompt);reader_calls++;
    if(interrupt_read) {app.kill_reason="test";assert(cancel(ctx));return INPUT_CONSOLE_INTERRUPTED;}
    memcpy(out,"abc",4);if(alter_pointer)live->rdi=0;return 3;
}

int main(void)
{
    app.pid=1;app.abi=2;app.state=PROC_READY;app.tid=7;strcpy(app.name,"probe");
    task.proc=&app;task.id=7;
    assert(user_console_grant(&app)==-VOS_EACCES);
    app.abi=3;assert(!user_console_grant(&app));assert(user_console_grant(&app)==-VOS_EACCES);
    struct exc_frame f={.rax=SYS_CONSOLE_READ,.rdi=(uint64_t)memory,.rsi=0};live=&f;
    assert(user_console_syscall(&app,&f)==-VOS_EACCES);app.state=PROC_RUNNING;
    assert(user_console_syscall(&app,&f)==-VOS_EINVAL);
    f.rsi=130;assert(user_console_syscall(&app,&f)==-VOS_EINVAL);
    f.rsi=4;f.rdi=0;assert(user_console_syscall(&app,&f)==-VOS_EFAULT && !reader_calls);
    f.rdi=(uint64_t)memory;f.rsi=2;memset(memory,0x5A,sizeof(memory));
    assert(user_console_syscall(&app,&f)==-VOS_EMSGSIZE && reader_calls==1 && memory[0]==0x5A);
    f.rsi=4;assert(user_console_syscall(&app,&f)==3 && reader_calls==1 && !memcmp(memory,"abc",4) && memory[4]==0x5A);
    alter_pointer=1;memset(memory,0x5A,sizeof(memory));
    assert(user_console_syscall(&app,&f)==-VOS_EFAULT && memory[0]==0x5A);
    alter_pointer=0;f.rdi=(uint64_t)memory;
    assert(user_console_syscall(&app,&f)==3 && reader_calls==2);
    puts("PASS: explicit grant, live owner, bounded validation, atomic capacity retry and post-wait frame revalidation");
    f.rax=SYS_CONSOLE_WRITE;f.rsi=257;assert(user_console_syscall(&app,&f)==-VOS_EINVAL);
    f.rsi=1;f.rdi=0;assert(user_console_syscall(&app,&f)==-VOS_EFAULT);
    f.rsi=0;assert(user_console_syscall(&app,&f)==0);
    memcpy(memory,"ok\033\n",4);f.rdi=(uint64_t)memory;f.rsi=4;
    assert(user_console_syscall(&app,&f)==4 && !memcmp(display,"ok?\n",4));
    assert(nl==strlen("user[probe]: ok?\r\n") && !memcmp(logtext,"user[probe]: ok?\r\n",nl));
    f.rax=SYS_CONSOLE_CLEAR;assert(!user_console_syscall(&app,&f) && clears==2);
    app.pid=2;assert(user_console_syscall(&app,&f)==-VOS_EACCES);app.pid=1;
    f.rax=SYS_CONSOLE_READ;f.rsi=4;interrupt_read=1;
    assert(user_console_syscall(&app,&f)==-VOS_EINTR);
    user_console_revoke(&app);app.kill_reason=0;
    assert(user_console_syscall(&app,&f)==-VOS_EACCES);
    puts("PASS: bounded/sanitized display writes, prefixed diagnostic isolation, clear, PID reuse, cancellation and revocation");
    return 0;
}
