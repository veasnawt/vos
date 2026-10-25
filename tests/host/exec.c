/* Host scheduler/loader model exercising the actual kernel execution broker.
 * Native process/ELF/scheduler behavior is verified separately in QEMU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "exec.h"
#include "exec_verify.h"
#include "process.h"
#include "exception.h"
#include "syscall.h"
#include "loader.h"
#include "panic.h"
#include "user_console.h"
int user_console_owned(const struct process *p){(void)p;return 0;}
int user_console_begin_exec(struct process *p,struct process *c){(void)p;(void)c;return -VOS_EACCES;}
int user_console_finish_exec(struct process *p,struct process *c,int r){(void)p;(void)c;(void)r;return 0;}
int user_console_exec_active(const struct process *p,const struct process *c){(void)p;(void)c;return 0;}
static unsigned assertions,loads,destroys,starts,kills;
static int benchmark;
#define CHECK(x) do {assertions++;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static struct process parent,other,*live_child;
static struct thread user_thread,worker;
static struct thread *current;
static uint64_t root,pid=20;
static int child_thread_state,load_failure,start_failure,inject,exit_value,fault_value;
static struct vos_app_entry_v1 output;
static uint8_t image[64]={0x7f,'E','L','F'};
static struct app_catalog catalog={.count=3,.valid=1,.entries={
 { .identity=0x1001,.name="vsh",.policy=APP_POLICY_SHELL,.image=image,.length=64 },
 { .identity=0x1002,.name="hello",.policy=APP_POLICY_DIAGNOSTIC,.image=image,.length=64 },
 { .identity=0x2001,.name="diagnostic",.policy=APP_POLICY_DIAGNOSTIC,.image=image,.length=64 }
}};
uint64_t exec_host_root(void){return root;}
uint64_t vmm_kernel_root(void){return 0x1000;}
struct thread *thread_current(void){return current;}
__attribute__((noreturn)) void panic(const char *p){fprintf(stderr,"PANIC %s\n",p);abort();}
int proc_file_live(const struct process *p){return p==&parent || p==&other || (p && p==live_child);}
int64_t thread_create(const char *n,thread_fn f,void *a){(void)n;(void)f;(void)a;worker.id=2;worker.state=THREAD_RUNNABLE;return 2;}
void thread_yield(void){worker.state=THREAD_FREE;}
int thread_state(uint64_t id){return id==2 ? worker.state : child_thread_state;}
int thread_wake_all(struct wait_queue *q){(void)q;return 0;}
void thread_block(struct wait_queue *q){
 (void)q;CHECK(current==&user_thread);struct thread *old=current;uint64_t r=root;
 current=&worker;root=0x1000;exec_host_pump();root=r;current=old;
}
int user_writable(uint64_t r,uint64_t p,size_t n){return r!=parent.root || p!=(uintptr_t)&output || n!=48 ? -3:0;}
int copy_to_user(uint64_t r,uint64_t p,const void *v,size_t n){
 if(user_writable(r,p,n))return -3;
 memcpy((void *)(uintptr_t)p,v,n);return 0;
}
const struct app_catalog_entry *app_catalog_at(const struct app_catalog *c,size_t i){return c&&c->valid&&i<c->count?&c->entries[i]:NULL;}
const struct app_catalog_entry *app_catalog_find(const struct app_catalog *c,uint64_t id){
 for(unsigned i=0;c&&i<c->count;i++)if(c->entries[i].identity==id)return &c->entries[i];
 return NULL;
}
int load_elf(struct process *p,const char *n,const uint8_t *b,uint64_t length,struct elf_image *im,int *e){
 (void)n;(void)length;(void)im;(void)e;CHECK(root==0x1000 && !live_child);
 if(load_failure)return load_failure;
 if(b!=image)return PROC_EEXEC;
 memset(p,0,sizeof(*p));p->pid=++pid;p->state=PROC_READY;p->abi=4;p->root=0x3000;
 live_child=p;loads++;return 0;
}
int proc_start(struct process *p){
 CHECK(root==0x1000 && live_child==p && p->state==PROC_READY);
 if(start_failure)return start_failure;
 p->state=PROC_RUNNING;p->tid=30;child_thread_state=THREAD_RUNNABLE;starts++;return 0;
}
int proc_wait(struct process *p){
 CHECK(root==0x1000 && live_child==p && p->state==PROC_RUNNING);
 CHECK(exec_syscall(p,&(struct exc_frame){.rax=SYS_APP_RUN_FOREGROUND,.rdi=0x1002})==-VOS_EACCES);
 p->state=p->kill_reason || fault_value ? PROC_KILLED:PROC_EXITED;
 p->exit_status=exit_value;p->trap.vector=(uint64_t)fault_value;
 child_thread_state=THREAD_FREE;return p->state;
}
void proc_destroy(struct process *p){
 CHECK(root==0x1000 && p==live_child && p->state!=PROC_RUNNING);
 CHECK(!p->tid || child_thread_state==THREAD_FREE);
 exec_process_end(p);live_child=NULL;p->state=PROC_FREE;destroys++;
}
void proc_kill(struct process *p,const char *why){CHECK(p==live_child);p->kill_reason=why;kills++;}
void exec_verify_source(const uint8_t **b,uint64_t *length){static const uint8_t bad[64]={0};if(inject==15){*b=bad;*length=64;}}
void exec_verify_event(enum exec_event e,struct process *p,struct process *child){
 (void)child;
 if((inject==4&&e==EXEC_EV_QUEUED)||(inject==5&&e==EXEC_EV_AFTER_LOAD)||
    (inject==18&&e==EXEC_EV_BEFORE_START)||(inject==7&&e==EXEC_EV_AFTER_START)||
    (inject==10&&e==EXEC_EV_AFTER_PUBLISH))exec_grant_revoke(p);
 if((inject==6&&e==EXEC_EV_AFTER_START)||(inject==8&&e==EXEC_EV_REAP)||
    (inject==9&&e==EXEC_EV_BEFORE_PUBLISH))exec_cancel_request(p);
 if(inject==16&&e==EXEC_EV_AFTER_START){
  struct thread t={.id=other.tid,.proc=&other};struct thread *saved=current;uint64_t r=root;
  current=&t;root=other.root;
  CHECK(exec_syscall(&other,&(struct exc_frame){.rax=15,.rdi=0x1002})==-VOS_EAGAIN);
  root=r;current=saved;
 }
 if(inject==17&&e==EXEC_EV_AFTER_START){p->kill_reason="parent killed";exec_process_cancel(p);}
}
static int64_t call(uint64_t number,uint64_t a,uint64_t b,uint64_t c){
 return exec_syscall(&parent,&(struct exc_frame){.rax=number,.rdi=a,.rsi=b,.rdx=c});
}
static void regrant(unsigned rights){
 const uint64_t ids[]={0x1002,0x2001};
 exec_grant_revoke(&parent);parent.kill_reason=NULL;CHECK(exec_test_regrant(&parent,rights,ids,2)==0);
}
static int64_t execute(int mode){
 inject=mode;int64_t r=call(15,0x1002,0,0);CHECK(!exec_request_busy());CHECK(!live_child);CHECK(loads==destroys);inject=0;return r;
}
static void tests(void){
 const uint64_t ids[]={0x1002,0x2001};
 root=0x1000;current=&worker;CHECK(exec_service_init(&catalog)==0);
 parent=(struct process){.pid=1,.abi=4,.state=PROC_READY,.root=0x2000};
 other=(struct process){.pid=2,.abi=4,.state=PROC_READY,.root=0x4000};
 CHECK(exec_grant_install(&parent,EXEC_RIGHTS,ids,2)==0);
 CHECK(exec_grant_install(&parent,EXEC_RIGHTS,ids,2)==-VOS_EAGAIN);
 CHECK(exec_grant_install(&other,EXEC_RUN,ids,2)==0);
 other.state=PROC_RUNNING;other.tid=12;
 parent.state=PROC_RUNNING;parent.tid=11;
 user_thread=(struct thread){.id=11,.state=THREAD_RUNNING,.proc=&parent};current=&user_thread;root=parent.root;
 CHECK(exec_grant_install(&parent,EXEC_RIGHTS,ids,2)==-VOS_EINVAL);
 for(unsigned abi=0;abi<4;abi++){parent.abi=abi;CHECK(call(14,0,(uintptr_t)&output,48)==-VOS_ENOSYS);CHECK(call(15,0x1002,0,0)==-VOS_ENOSYS);}parent.abi=4;
 CHECK(exec_syscall((void *)(uintptr_t)1,&(struct exc_frame){.rax=14})==-VOS_EACCES);
 CHECK(call(14,0,(uintptr_t)&output,47)==-VOS_EINVAL);CHECK(call(14,0,(uintptr_t)&output,49)==-VOS_EINVAL);
 CHECK(call(14,0,1,48)==-VOS_EFAULT);
 memset(&output,0x5a,sizeof(output));CHECK(call(14,0,(uintptr_t)&output,48)==48);
 CHECK(output.app_id==0x1002 && output.size==48 && output.flags==0);
 CHECK(output.name_length==5 && !memcmp(output.name,"hello",6));for(unsigned i=6;i<16;i++)CHECK(!output.name[i]);
 uint64_t old=output.next_cookie;
 CHECK(call(14,(old&~15ULL)|15,(uintptr_t)&output,48)==-VOS_EINVAL);
 CHECK(call(14,old,(uintptr_t)&output,48)==48 && output.app_id==0x2001);
 uint64_t end=output.next_cookie;struct vos_app_entry_v1 saved=output;
 CHECK(call(14,end,(uintptr_t)&output,48)==0 && !memcmp(&saved,&output,48));
 CHECK(call(14,end,1,48)==-VOS_EFAULT);
 CHECK(call(15,0,0,0)==-VOS_EINVAL);CHECK(call(15,0x1002,1,0)==-VOS_EINVAL);
 CHECK(call(15,0x1001,0,0)==-VOS_EACCES);CHECK(call(15,0xbeef,0,0)==-VOS_EACCES);
 regrant(EXEC_ENUM);CHECK(call(15,0x1002,0,0)==-VOS_EACCES);
 regrant(EXEC_RUN);CHECK(call(14,0,(uintptr_t)&output,48)==-VOS_EACCES);
 regrant(EXEC_RIGHTS);CHECK(call(14,old,(uintptr_t)&output,48)==-VOS_ESTALE);
 CHECK(execute(0)==0);exit_value=-7;CHECK(execute(0)==(int64_t)(uint32_t)-7);exit_value=0;
 fault_value=14;CHECK(execute(0)==((1LL<<32)|14));fault_value=0;
 const int pre[]={4,5,18};for(unsigned i=0;i<3;i++){CHECK(execute(pre[i])==-VOS_EINTR);regrant(EXEC_RIGHTS);}
 const int failures[]={PROC_ENOMEM,PROC_EBUSY,PROC_EEXEC};
 const int expected[]={-VOS_ENOMEM,-VOS_EAGAIN,-VOS_EUNSUPP};
 for(unsigned i=0;i<3;i++){load_failure=failures[i];CHECK(execute(0)==expected[i]);}load_failure=0;
 start_failure=PROC_ENOMEM;CHECK(execute(0)==-VOS_ENOMEM);start_failure=0;
 CHECK(execute(15)==-VOS_EUNSUPP);
 const int cancel[]={6,7,8,9,17};
 for(unsigned i=0;i<5;i++){CHECK(execute(cancel[i])==((2LL<<32)|1));regrant(EXEC_RIGHTS);}
 CHECK(execute(16)==0);CHECK(execute(10)==0);CHECK(call(15,0x1002,0,0)==-VOS_EACCES);regrant(EXEC_RIGHTS);
 parent.pid++;CHECK(call(15,0x1002,0,0)==-VOS_EACCES);regrant(EXEC_RIGHTS);
 for(unsigned i=0;i<1000;i++)CHECK(execute(0)==0);
 const struct exec_stats *s=exec_statistics();CHECK(s->loaded==s->reclaimed);CHECK(s->submitted==s->completed);
 CHECK(s->cancelled==5 && s->failed==8 && kills>=3);
 uint64_t completed=s->completed;
 exec_grant_revoke(&parent);exec_grant_revoke(&other);root=0x1000;current=&worker;
 CHECK(exec_service_stop()==0);CHECK(exec_service_init(&catalog)==0);
 root=parent.root;current=&user_thread;regrant(EXEC_RIGHTS);
 CHECK(call(14,old,(uintptr_t)&output,48)==-VOS_ESTALE);
 exec_grant_revoke(&parent);root=0x1000;current=&worker;CHECK(exec_service_stop()==0);
 if(!benchmark)printf("PASS assertions=%u jobs=%llu loaded=%u started=%u reclaimed=%u; actual exec.c, modeled scheduler/loader\n",assertions,(unsigned long long)completed,loads,starts,destroys);
}
int main(int argc,char **argv){
 benchmark=argc==2 && !strcmp(argv[1],"--bench");
 if(benchmark){
  printf("{\"scope\":\"optimized host broker plus modeled lifecycle; not native latency\",\"jobs_per_sample\":1000,\"sample_total_ns\":[");
  for(unsigned i=0;i<7;i++){
   struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);tests();clock_gettime(CLOCK_MONOTONIC,&b);
   printf("%s%llu",i?",":"",(unsigned long long)((b.tv_sec-a.tv_sec)*1000000000ULL+b.tv_nsec-a.tv_nsec));
  }
  puts("]}");
 }else tests();return 0;
}
