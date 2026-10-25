/* Explicit verification bootstrap, never a public grant/launch mechanism. */
#include "exec_verify.h"
#include "handoff_verify.h"
#include "exec.h"
#include "bootinfo.h"
#include "app_payload.h"
#include "loader.h"
#include "process.h"
#include "thread.h"
#include "pmm.h"
#include "resv.h"
#include "memlayout.h"
#include "console.h"
#include "screen.h"
#include "syscall.h"
#include "exception.h"
#include "lib.h"
#include "panic.h"

static const struct boot_info *boot;
static struct app_catalog test_catalog;
static struct process parent, contender, fillers[PROC_TABLE];
static uint64_t filler_threads[THREAD_MAX], parent_pid;
static unsigned filler_count, thread_count_used, mode, control_count;
static int verifying;
static uint64_t baseline_free;
static unsigned baseline_processes, baseline_threads;
static const uint64_t ids[]={0x1002,0x2001,0x2002,0x2003,0x2004};
static const char *const modules[]={
 "vsh","hello","rixie","exec-probe","exec-negative","exec-fault",
 "exec-spin","exec-recursive","exec-contender","exec-legacy0",
 "exec-legacy1","exec-legacy2","exec-legacy3"
};
static void display(const char *p) {while(*p)screen_putc(*p++);}
static void need(int ok,const char *why) {if(!ok)panic(why);}
int exec_verify_requested(const char *p)
{
    const char *token="vos.exec=1";
    if (!p) return 0;
    while (*p) {
        while (*p==' ' || *p=='\t') p++;
        unsigned i=0;while(token[i] && p[i]==token[i])i++;
        if(!token[i] && (!p[i] || p[i]==' ' || p[i]=='\t'))return 1;
        while(*p && *p!=' ' && *p!='\t')p++;
    }
    return 0;
}
static const struct boot_module *module(const char *name)
{
    const struct boot_module *found=NULL;
    for(unsigned i=0;i<boot->module_count;i++)
        if(strlen(boot->modules[i].cmdline)==strlen(name) &&
           !memcmp(boot->modules[i].cmdline,name,strlen(name))) {
            if(found)return NULL;
            found=&boot->modules[i];
        }
    return found;
}
static int load_module(struct process *p,const char *name)
{
    const struct boot_module *m=module(name);struct elf_image image;int error;
    return m ? load_elf(p,name,phys_to_virt(m->start),m->end-m->start,&image,&error) : PROC_EEXEC;
}
int exec_verify_prepare(const struct boot_info *bi,const struct resv_table *reserved)
{
    boot=bi;
    unsigned extra=0;
#ifdef VOS_HANDOFF_VERIFY
    extra=handoff_verify_requested(bi->cmdline) ? 1 : 0;
#endif
    if(bi->module_count!=sizeof(modules)/sizeof(modules[0])+extra)return 0;
    if(extra) {
        const struct boot_module *m=module("handoff-probe");
        if(!m || m->start>=m->end || m->end>BOOT_MAP_LIMIT || m->end-m->start>ELF_MAX_FILE)return 0;
        for(uint64_t p=m->start & ~(PAGE_SIZE-1);p<m->end;p+=PAGE_SIZE)
            if(!resv_overlaps(reserved,p,p+PAGE_SIZE))return 0;
    }
    for(unsigned i=0;i<sizeof(modules)/sizeof(modules[0]);i++) {
        const struct boot_module *m=module(modules[i]);
        if(!m || m->start>=m->end || m->end>BOOT_MAP_LIMIT || m->end-m->start>ELF_MAX_FILE)return 0;
        for(uint64_t p=m->start & ~(PAGE_SIZE-1);p<m->end;p+=PAGE_SIZE)
            if(!resv_overlaps(reserved,p,p+PAGE_SIZE))return 0;
    }
    struct boot_info normal=*bi;normal.module_count=3;
    for(unsigned i=0;i<3;i++)normal.modules[i]=*module(modules[i]);
    if(!app_payload_prepare(&normal,reserved))return 0;
    struct app_source source[7]={0};
    for(unsigned i=0;i<3;i++) {
        const struct app_catalog_entry *e=app_catalog_at(app_payload_catalog(),i);
        source[i].version=1;source[i].policy=e->policy;source[i].identity=e->identity;
        memcpy(source[i].name,e->name,16);source[i].image=e->image;source[i].length=e->length;
    }
    for(unsigned i=0;i<4;i++) {
        const char *name=modules[4+i];const struct boot_module *m=module(name);
        source[3+i].version=1;source[3+i].policy=APP_POLICY_DIAGNOSTIC;
        source[3+i].identity=0x2001+i;memcpy(source[3+i].name,name,strlen(name));
        source[3+i].image=phys_to_virt(m->start);source[3+i].length=m->end-m->start;
    }
    return app_catalog_build(&test_catalog,source,7)==0;
}
static void audit_parent(void)
{
    need(!exec_request_busy(),"exec verify: stale request");
    need(pmm_free_count()==baseline_free,"exec verify: leaked frame after request");
    need(proc_count()==baseline_processes && thread_count()==baseline_threads,
         "exec verify: leaked process/thread after request");
    need(!proc_audit() && !sched_audit() && thread_canaries_intact(),"exec verify: invariant audit");
}
int64_t exec_verify_syscall(struct process *p,const struct exc_frame *f)
{
    struct thread *t=thread_current();
    if(p!=&parent || !proc_file_live(p) || p->pid!=parent_pid || p->abi!=4 ||
       p->state!=PROC_RUNNING || !t || t->proc!=p || t->id!=p->tid)
        return -VOS_EACCES;
    if(f->rdi>18 || f->rsi)return -VOS_EINVAL;
    audit_parent();mode=(unsigned)f->rdi;control_count++;
    exec_grant_revoke(p);
    if(mode==3)return 0;
    uint32_t rights=mode==1 ? EXEC_ENUM : mode==2 ? EXEC_RUN : EXEC_RIGHTS;
    int result=exec_test_regrant(p,rights,ids,mode==1 || mode==2 ? 1 : 5);
    need(!result,"exec verify: trusted refresh");
    return 0;
}
static void empty_thread(void *p) {(void)p;}
static void cleanup(void)
{
    proc_test_frame_budget(-1);
    while(filler_count)proc_destroy(&fillers[--filler_count]);
    for(unsigned i=0;i<thread_count_used;i++)
        while(thread_state(filler_threads[i])!=THREAD_FREE)thread_yield();
    thread_count_used=0;
}
void exec_verify_source(const uint8_t **bytes,uint64_t *length)
{
    #ifdef VOS_HANDOFF_VERIFY
    if(handoff_verify_active()){handoff_verify_source(bytes,length);return;}
#endif
    if(!verifying)return;
    static const uint8_t bad[64]={0};
    if(mode==15){*bytes=bad;*length=sizeof(bad);}
}
void exec_verify_event(enum exec_event event,struct process *p,struct process *child)
{
    #ifdef VOS_HANDOFF_VERIFY
    if(handoff_verify_active()){handoff_verify_event(event,p,child);return;}
#endif
    if(!verifying)return;
    if(event==EXEC_EV_QUEUED && mode==4)exec_grant_revoke(p);
    if(event==EXEC_EV_BEFORE_LOAD) {
        if(mode==11)proc_test_frame_budget(0);
        if(mode==12)proc_test_frame_budget(3);
        if(mode==13) {
            static const uint8_t code[]={0xc3};
            while(proc_count()<PROC_TABLE) {
                need(filler_count<PROC_TABLE,"exec verify: filler bound");
                need(proc_create(&fillers[filler_count],"exec-filler",code,sizeof(code))==0,
                     "exec verify: process filler allocation");
                filler_count++;
            }
        }
    }
    if(event==EXEC_EV_AFTER_LOAD) {
        if(mode==5)exec_grant_revoke(p);
        if(mode==14) {
            for(;;) {
                int64_t id=thread_create("exec-filler",empty_thread,NULL);
                if(id<=0)break;
                need(thread_count_used<THREAD_MAX,"exec verify: thread filler bound");
                filler_threads[thread_count_used++]=(uint64_t)id;
            }
        }
    }
    if(event==EXEC_EV_BEFORE_START && mode==18)exec_grant_revoke(p);
    if(event==EXEC_EV_AFTER_START) {
        if(mode==6 || mode==7 || mode==16 || mode==17)thread_sleep(2);
        if(mode==6)exec_cancel_request(p);
        if(mode==7)exec_grant_revoke(p);
        if(mode==16) {
            need(load_module(&contender,"exec-contender")==0,"exec verify: contender load");
            uint64_t hello=0x1002;
            need(exec_grant_install(&contender,EXEC_RUN,&hello,1)==0,"exec verify: contender grant");
            need(proc_run(&contender)==PROC_EXITED && contender.exit_status==0,
                 "exec verify: native one-slot contention");
            proc_destroy(&contender);exec_cancel_request(p);
        }
        if(mode==17)proc_kill(p,"exec verify parent termination");
    }
    if(event==EXEC_EV_REAP && mode==8)exec_cancel_request(p);
    if(event==EXEC_EV_CLEANUP)cleanup();
    if(event==EXEC_EV_BEFORE_PUBLISH && mode==9)exec_cancel_request(p);
    if(event==EXEC_EV_AFTER_PUBLISH && mode==10)exec_grant_revoke(p);
    (void)child;
}
int exec_verify_run(void)
{
    verifying=1;
    uint64_t initial_free=pmm_free_count();
    unsigned initial_processes=proc_count(),initial_threads=thread_count();
    need(exec_service_init(&test_catalog)==0,"exec verify: broker start");
    for(unsigned i=0;i<4;i++) {
        const char *name=modules[9+i];
        need(load_module(&parent,name)==0,"exec verify: legacy load");
        need(proc_run(&parent)==PROC_EXITED && parent.exit_status==0,"exec verify: ABI v0-v3 compatibility");
        proc_destroy(&parent);
        need(pmm_free_count()==initial_free,"exec verify: legacy cleanup");
    }
    need(load_module(&parent,"exec-probe")==0,"exec verify: Rust parent load");
    need(exec_grant_install(&parent,EXEC_RIGHTS,ids,5)==0,"exec verify: bootstrap grant");
    parent_pid=parent.pid;baseline_free=pmm_free_count();
    baseline_processes=proc_count();baseline_threads=thread_count()+1;
    mode=0;
    int state=proc_run(&parent);
    need(state==PROC_KILLED && parent.kill_reason &&
         !memcmp(parent.kill_reason,"exec verify parent termination",sizeof("exec verify parent termination")),
         "exec verify: parent cancellation/termination");
    need(control_count>=25,"exec verify: incomplete Rust probe");
    need(!exec_request_busy(),"exec verify: orphaned child request");
    proc_destroy(&parent);parent_pid=0;
    struct exec_stats stats=*exec_statistics();
    need(stats.loaded==stats.reclaimed && stats.started<=stats.loaded && stats.submitted==stats.completed &&
         stats.started>=15 && stats.cancelled>=5 && stats.failed>=7,"exec verify: lifecycle counts");
    need(exec_service_stop()==0,"exec verify: broker shutdown");
    need(proc_count()==initial_processes && thread_count()==initial_threads &&
         pmm_free_count()==initial_free && !proc_audit() && !sched_audit() &&
         thread_canaries_intact(),"exec verify: final cleanup audit");
    verifying=0;
    serial_puts("exec-verify: PASS legacy=4 controls=");serial_dec(control_count);
    serial_puts(" submitted=");serial_dec(stats.submitted);
    serial_puts(" started=");serial_dec(stats.started);
    serial_puts(" reclaimed=");serial_dec(stats.reclaimed);
    serial_puts(" cancelled=");serial_dec(stats.cancelled);
    serial_puts(" failed=");serial_dec(stats.failed);
    serial_puts(" PMM/process/thread restored; kernel CR3 reaping\r\n");
    screen_begin_input();
    display("ABI v4 execution authority and kernel broker\n");
    display("Rust ring-3 parent -> isolated native hello: PASS\n");
    display("Negative exit, child fault, recursion denial: PASS\n");
    display("Revocation, cancellation, contention, rollback: PASS\n");
    display("Parent terminated only after child reclamation: PASS\n");
    display("PMM, process and thread audits restored: PASS\n");
    display("Legacy probe held no console or execution-inheritance grants.\n");
    screen_status(SCREEN_PASS,"ABI v0-v4 non-console execution verified");
    serial_puts("exec-verify: SCREEN READY\r\n");
    thread_sleep(300); /* Fixture-only capture dwell, never an application deadline. */
    return 1;
}
