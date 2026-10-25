/* Native console-lease failure injection. Absent from normal builds. */
#include "handoff_verify.h"
#include "exec_verify.h"
#include "exec.h"
#include "app_payload.h"
#include "bootinfo.h"
#include "loader.h"
#include "process.h"
#include "thread.h"
#include "user_console.h"
#include "pmm.h"
#include "memlayout.h"
#include "exception.h"
#include "syscall.h"
#include "console.h"
#include "lib.h"
#include "panic.h"
static const struct boot_info *boot;
static struct app_catalog catalog;
static struct process parent,contender,fillers[PROC_TABLE];
static uint64_t filler_threads[THREAD_MAX],parent_pid;
static unsigned mode,filler_count,thread_fill;
static int active;
static const uint64_t ids[]={0x1002,0x1003,0x2001,0x2002,0x2003,0x2004};
static void need(int x,const char *m){if(!x)panic(m);}
int handoff_verify_requested(const char *p)
{
    if(!p)return 0;
    const char *token="vos.handoff=1";
    while(*p) {
        while(*p==' ' || *p=='\t')p++;
        unsigned n=0;while(token[n] && p[n]==token[n])n++;
        if(!token[n] && (!p[n] || p[n]==' ' || p[n]=='\t'))return 1;
        while(*p && *p!=' ' && *p!='\t')p++;
    }
    return 0;
}
int handoff_verify_active(void){return active;}
static const struct boot_module *module(const char *name)
{
    const struct boot_module *found=0;
    for(unsigned i=0;i<boot->module_count;i++)
        if(strlen(boot->modules[i].cmdline)==strlen(name) &&
           !memcmp(boot->modules[i].cmdline,name,strlen(name))) {
            need(!found,"handoff verify duplicate module");found=&boot->modules[i];
        }
    return found;
}
static int load(struct process *p,const char *name)
{
    const struct boot_module *m=module(name);struct elf_image image;int error;
    return m?load_elf(p,name,phys_to_virt(m->start),m->end-m->start,&image,&error):PROC_EEXEC;
}
int64_t handoff_verify_syscall(struct process *p,const struct exc_frame *f)
{
    struct thread *t=thread_current();
    if(!active || p!=&parent || !proc_file_live(p) || p->pid!=parent_pid ||
       !t || t->proc!=p || t->id!=p->tid)return -VOS_EACCES;
    if(f->rdi || f->rsi)return -VOS_EINVAL;
    return mode;
}
static void empty(void *p){(void)p;}
static void cleanup(void)
{
    proc_test_frame_budget(-1);
    while(filler_count)proc_destroy(&fillers[--filler_count]);
    for(unsigned i=0;i<thread_fill;i++)
        while(thread_state(filler_threads[i])!=THREAD_FREE)thread_yield();
    thread_fill=0;
}
void handoff_verify_source(const uint8_t **p,uint64_t *length)
{
    static const uint8_t bad[64]={0};
    if(mode==13){*p=bad;*length=sizeof(bad);}
}
void handoff_verify_event(enum exec_event event,struct process *p,struct process *child)
{
    if(event==EXEC_EV_QUEUED && mode==6)exec_grant_revoke(p);
    if(event==EXEC_EV_BEFORE_LOAD) {
        if(mode==10)proc_test_frame_budget(0);
        if(mode==11) {
            static const uint8_t code[]={0xc3};
            while(proc_count()<PROC_TABLE) {
                need(filler_count<PROC_TABLE,"handoff filler bound");
                need(proc_create(&fillers[filler_count],"handoff-fill",code,1)==0,"handoff process filler");
                filler_count++;
            }
        }
    }
    if(event==EXEC_EV_AFTER_LOAD) {
        if(mode==0) {
            serial_puts("handoff-verify: QUEUE PRE-HANDOFF\r\n");
            thread_sleep(100); /* Verification-only input injection window. */
        }
        if(mode==12) {
            for(;;) {
                int64_t id=thread_create("handoff-fill",empty,0);if(id<=0)break;
                need(thread_fill<THREAD_MAX,"handoff thread bound");
                filler_threads[thread_fill++]=(uint64_t)id;
            }
        }
    }
    if(event==EXEC_EV_AFTER_HANDOFF && mode==14)user_console_revoke(p);
    if(event==EXEC_EV_AFTER_START) {
        if(mode==4 || mode==7 || mode==8 || mode==9 || mode==16)thread_sleep(2);
        if(mode==4) {
            need(load(&contender,"exec-contender")==0,"handoff contender load");
            uint64_t hello=0x1002;
            need(exec_grant_install(&contender,EXEC_RUN,&hello,1)==0,"handoff contender grant");
            need(proc_run(&contender)==PROC_EXITED && contender.exit_status==0,"handoff contention");
            proc_destroy(&contender);exec_cancel_request(p);
        }
        if(mode==7)user_console_revoke(p);
        if(mode==8)proc_kill(p,"handoff verify parent terminated");
        if(mode==9){need(user_console_reader_active(child),"handoff child reader did not block");user_console_revoke(child);}
        if(mode==16)exec_grant_revoke(p);
    }
    if(event==EXEC_EV_REAP && mode==5)exec_cancel_request(p);
    if(event==EXEC_EV_REAP && mode==15)user_console_revoke(p);
    if(event==EXEC_EV_CLEANUP)cleanup();
}
int handoff_verify_run(const struct boot_info *bi)
{
    boot=bi;active=1;
    const char *names[]={"exec-negative","exec-fault","exec-spin","exec-recursive"};
    struct app_source sources[7]={0};
    for(unsigned i=0;i<3;i++) {
        const struct app_catalog_entry *e=app_catalog_at(app_payload_catalog(),i);
        need(e!=0,"handoff base catalog");
        sources[i].version=1;sources[i].identity=e->identity;sources[i].policy=e->policy;
        memcpy(sources[i].name,e->name,16);sources[i].image=e->image;sources[i].length=e->length;
    }
    for(unsigned i=0;i<4;i++) {
        const struct boot_module *m=module(names[i]);need(m!=0,"handoff diagnostic source");
        sources[3+i].version=1;sources[3+i].identity=0x2001+i;sources[3+i].policy=APP_POLICY_DIAGNOSTIC;
        memcpy(sources[3+i].name,names[i],strlen(names[i]));
        sources[3+i].image=phys_to_virt(m->start);sources[3+i].length=m->end-m->start;
    }
    need(app_catalog_build(&catalog,sources,7)==0,"handoff catalog");
    uint64_t initial_free=pmm_free_count();unsigned initial_p=proc_count(),initial_t=thread_count();
    struct console_lease_stats before=*user_console_lease_stats();
    need(exec_service_init(&catalog)==0,"handoff broker start");
    uint64_t free=pmm_free_count();unsigned processes=proc_count(),threads=thread_count();
    for(mode=0;mode<=20;mode++) {
        need(load(&parent,"handoff-probe")==0,"handoff parent load");
        parent_pid=parent.pid;
        unsigned rights=mode==18?EXEC_ENUM:mode==19?(EXEC_RUN|EXEC_FOREGROUND):EXEC_ALL;
        need(exec_grant_install(&parent,rights,ids,6)==0,"handoff explicit grant");
        need(user_console_grant(&parent)==0,"handoff parent console");
        int state=proc_run(&parent);
        need((mode==8 && state==PROC_KILLED && parent.kill_reason &&
              !memcmp(parent.kill_reason,"handoff verify parent terminated",sizeof("handoff verify parent terminated"))) ||
             (mode!=8 && state==PROC_EXITED && parent.exit_status==0),"handoff parent result");
        proc_destroy(&parent);parent_pid=0;
        need(!exec_request_busy() && !user_console_lease_active(),"handoff stale request/lease");
        need(pmm_free_count()==free && proc_count()==processes && thread_count()==threads,
             "handoff resource leak");
        need(!proc_audit() && !sched_audit() && thread_canaries_intact(),"handoff invariant audit");
        serial_puts("handoff-verify: CASE ");serial_dec(mode);serial_puts(" PASS; resources restored\r\n");
    }
    struct exec_stats s=*exec_statistics();
    struct console_lease_stats after=*user_console_lease_stats();
    need(s.submitted==20 && s.completed==20 && s.loaded==16 && s.reclaimed==16 &&
         s.started==14 && s.cancelled==6 && s.failed==6,"handoff lifecycle counts");
    need(after.transferred-before.transferred==16 && after.restored-before.restored==11 &&
         after.abandoned-before.abandoned==5,"handoff ownership counts");
    need(exec_service_stop()==0,"handoff broker join");active=0;
    need(pmm_free_count()==initial_free && proc_count()==initial_p && thread_count()==initial_t,
         "handoff final resource audit");
    serial_puts("handoff-verify: PASS cases=21 requests=20 loaded=16 started=14 reclaimed=16 cancelled=6 failed=6 transfers=16 restores=11 abandoned=5; kernel CR3, PMM/process/thread audits restored\r\n");
    return 1;
}
