#include "exec.h"
#include "exec_verify.h"
#include "process.h"
#include "thread.h"
#include "syscall.h"
#include "exception.h"
#include "loader.h"
#include "lib.h"
#include "panic.h"
#include "cpu.h"
#include "vmm.h"
#include "usercopy.h"

enum job_state { JOB_FREE, JOB_QUEUED, JOB_LOADING, JOB_RUNNING, JOB_REAPING, JOB_DONE };
struct authority {
    struct process *owner;
    uint64_t pid, generation;
    uint32_t rights, count;
    uint64_t ids[APP_CATALOG_MAX];
};
static struct authority grants[PROC_TABLE];
static uint64_t next_generation = 1;
static const struct app_catalog *catalog;
static uint64_t worker_id;
static int stopping;
static struct wait_queue requests;
static struct exec_stats statistics;
static struct {
    enum job_state state;
    struct process *parent;
    uint64_t pid, tid, generation, app_id;
    int cancel, loaded, started;
    int64_t result;
    struct wait_queue completion;
    struct process child;
} job;

static uint64_t current_root(void)
{
#ifdef VOS_EXEC_HOST
    return exec_host_root();
#else
    return read_cr3() & PTE_ADDR;
#endif
}
static void kernel_context(void)
{
    if (current_root() != vmm_kernel_root())
        panic("exec: broker is not in kernel address space");
#ifndef VOS_EXEC_HOST
    if (read_rflags() & (1ULL << 9)) panic("exec: interrupts enabled in kernel");
#endif
}
static void event(enum exec_event e)
{
#if defined(VOS_EXEC_VERIFY) || defined(VOS_EXEC_HOST)
    exec_verify_event(e, job.parent, &job.child);
#else
    (void)e;
#endif
}
static struct authority *authority(struct process *p)
{
    if (!proc_file_live(p)) return NULL;
    for (unsigned i=0;i<PROC_TABLE;i++)
        if (grants[i].owner==p && grants[i].pid==p->pid) return &grants[i];
    return NULL;
}
static int listed(const struct authority *a,uint64_t id)
{
    for (unsigned i=0;i<a->count;i++) if (a->ids[i]==id) return 1;
    return 0;
}
static int parent_matches(struct process *p)
{
    return job.state!=JOB_FREE && job.parent==p && p && job.pid==p->pid;
}
int exec_request_busy(void) { return job.state!=JOB_FREE; }
const struct exec_stats *exec_statistics(void) { return &statistics; }

void exec_cancel_request(struct process *p)
{
    if (!parent_matches(p) || job.state==JOB_DONE) return;
    job.cancel=1;
    if (job.started && job.child.state==PROC_RUNNING)
        proc_kill(&job.child,"execution cancelled");
    thread_wake_all(&requests);
    thread_wake_all(&job.completion);
}
void exec_grant_revoke(struct process *p)
{
    if (!p) return;
    for (unsigned i=0;i<PROC_TABLE;i++)
        if (grants[i].owner==p && grants[i].pid==p->pid)
            memset(&grants[i],0,sizeof(grants[i]));
    exec_cancel_request(p);
}
void exec_process_cancel(struct process *p) { exec_grant_revoke(p); }
void exec_process_end(struct process *p)
{
    if (parent_matches(p)) panic("exec: ending a parent before its child was joined");
    exec_grant_revoke(p);
}

static int install(struct process *p,uint32_t rights,const uint64_t *ids,unsigned count,int verification_refresh)
{
    if (!worker_id || stopping) return -VOS_EUNAVAIL;
    int setup=proc_file_live(p) && p->state==PROC_READY && !p->tid;
#if defined(VOS_EXEC_VERIFY) || defined(VOS_EXEC_HOST)
    if (verification_refresh && proc_file_live(p)) {
        struct thread *t=thread_current();
        setup=p->state==PROC_RUNNING && t && t->proc==p && t->id==p->tid && current_root()==p->root;
    }
#else
    (void)verification_refresh;
#endif
    if (!setup || p->abi!=4 ||
        p->kill_reason || !rights || (rights & ~EXEC_RIGHTS) ||
        count>APP_CATALOG_MAX || (count && !ids))
        return -VOS_EINVAL;
    if (parent_matches(p)) return -VOS_EAGAIN;
    unsigned slot=PROC_TABLE;
    for (unsigned i=0;i<PROC_TABLE;i++) {
        if (grants[i].owner==p) {
            if (grants[i].pid==p->pid) return -VOS_EAGAIN;
            /* A recycled address never revives the old PID's authority. */
            memset(&grants[i],0,sizeof(grants[i]));
        }
        if (!grants[i].owner && slot==PROC_TABLE) slot=i;
    }
    if (slot==PROC_TABLE || next_generation>(UINT64_MAX>>4)) return -VOS_ENOMEM;
    for (unsigned i=0;i<count;i++) {
        const struct app_catalog_entry *e=app_catalog_find(catalog,ids[i]);
        if (!e) return -VOS_EINVAL;
        if ((rights & EXEC_RUN) && e->policy!=APP_POLICY_DIAGNOSTIC) return -VOS_EUNSUPP;
        for (unsigned j=0;j<i;j++) if (ids[j]==ids[i]) return -VOS_EINVAL;
    }
    struct authority a={.owner=p,.pid=p->pid,.generation=next_generation++,
                        .rights=rights,.count=count};
    for (unsigned i=0;i<count;i++) a.ids[i]=ids[i];
    grants[slot]=a;
    return 0;
}

int exec_grant_install(struct process *p,uint32_t rights,const uint64_t *ids,unsigned count)
{
    return install(p,rights,ids,count,0);
}
#if defined(VOS_EXEC_VERIFY) || defined(VOS_EXEC_HOST)
int exec_test_regrant(struct process *p,uint32_t rights,const uint64_t *ids,unsigned count)
{
    return install(p,rights,ids,count,1);
}
#endif

static int valid_parent(void)
{
    struct authority *a=authority(job.parent);
    return a && job.parent->state==PROC_RUNNING && job.parent->pid==job.pid &&
           job.parent->tid==job.tid && !job.parent->kill_reason &&
           a->generation==job.generation && (a->rights & EXEC_RUN) && listed(a,job.app_id);
}
static int64_t load_error(int e)
{
    return e==PROC_ENOMEM ? -VOS_ENOMEM : e==PROC_EBUSY ? -VOS_EAGAIN :
           e==PROC_EEXEC ? -VOS_EUNSUPP : -VOS_EINVAL;
}
static void process_one(void)
{
    kernel_context();
    if (job.state!=JOB_QUEUED) panic("exec: invalid request state");
    job.state=JOB_LOADING;
    int64_t result=-VOS_EINTR;
    int kind=VOS_APP_FAULTED;
    uint32_t status=0;
    event(EXEC_EV_BEFORE_LOAD);
    if (!job.cancel && valid_parent()) {
        const struct app_catalog_entry *e=app_catalog_find(catalog,job.app_id);
        struct elf_image image;
        int elf_error=0;
        const uint8_t *bytes=e ? e->image : NULL;
        uint64_t length=e ? e->length : 0;
#if defined(VOS_EXEC_VERIFY) || defined(VOS_EXEC_HOST)
        exec_verify_source(&bytes,&length);
#endif
        int r=e ? load_elf(&job.child,e->name,bytes,length,&image,&elf_error) : PROC_EEXEC;
        if (r!=PROC_OK) result=load_error(r); /* load_elf already rolled back. */
        else {
            job.loaded=1;statistics.loaded++;
            event(EXEC_EV_AFTER_LOAD);
            event(EXEC_EV_BEFORE_START);
            if (!job.cancel && valid_parent()) {
                /* Fresh proc_begin installs no channel, file, console,
                 * network or execution grants. There is no inheritance. */
                r=proc_start(&job.child);
                if (r!=PROC_OK) result=load_error(r);
                else {
                    job.started=1;job.state=JOB_RUNNING;statistics.started++;
                    event(EXEC_EV_AFTER_START);
                    int state=proc_wait(&job.child);
                    kernel_context();
                    if (thread_state(job.child.tid)!=THREAD_FREE)
                        panic("exec: child thread not reaped");
                    kind=state==PROC_EXITED ? VOS_APP_EXITED : VOS_APP_FAULTED;
                    status=state==PROC_EXITED ? (uint32_t)job.child.exit_status :
                           (uint32_t)job.child.trap.vector;
                    job.state=JOB_REAPING;
                    event(EXEC_EV_REAP);
                }
            }
        }
    }
    kernel_context();
    if (job.loaded) {
        proc_destroy(&job.child);job.loaded=0;statistics.reclaimed++;
    }
    event(EXEC_EV_CLEANUP);
    event(EXEC_EV_BEFORE_PUBLISH);
    if (job.started) {
        if (job.cancel) {kind=VOS_APP_CANCELLED;status=VOS_APP_CANCEL_KERNEL;statistics.cancelled++;}
        result=(int64_t)(((uint64_t)(unsigned)kind<<32)|status);
    } else {
        if (job.cancel) result=-VOS_EINTR;
        statistics.failed++;
    }
    job.result=result;
    job.state=JOB_DONE;statistics.completed++;
    event(EXEC_EV_AFTER_PUBLISH); /* DONE is the immutable completion point. */
    thread_wake_all(&job.completion);
}
#ifdef VOS_EXEC_HOST
void exec_host_pump(void) { process_one(); }
#endif
static void broker(void *unused)
{
    (void)unused;
    for (;;) {
        while (job.state!=JOB_QUEUED && !stopping) thread_block(&requests);
        if (stopping) return;
        process_one();
    }
}
int exec_service_init(const struct app_catalog *c)
{
    kernel_context();
    if (worker_id || !c || c->valid!=1 || !c->count || c->count>APP_CATALOG_MAX)
        return -VOS_EINVAL;
    catalog=c;stopping=0;
    memset(&job,0,sizeof(job));memset(&grants,0,sizeof(grants));
    memset(&requests,0,sizeof(requests));memset(&statistics,0,sizeof(statistics));
    int64_t id=thread_create("exec-broker",broker,NULL);
    if (id<=0) {catalog=NULL;return -VOS_ENOMEM;}
    worker_id=(uint64_t)id;
    return 0;
}
int exec_service_stop(void)
{
    kernel_context();
    if (!worker_id) return 0;
    if (job.state!=JOB_FREE) return -VOS_EAGAIN;
    for (unsigned i=0;i<PROC_TABLE;i++) if (grants[i].owner) return -VOS_EAGAIN;
    stopping=1;thread_wake_all(&requests);
    while (thread_state(worker_id)!=THREAD_FREE) thread_yield();
    worker_id=0;catalog=NULL;
    return 0;
}

int64_t exec_syscall(struct process *p,const struct exc_frame *f)
{
    if (!proc_file_live(p)) return -VOS_EACCES;
    if (p->abi!=4) return -VOS_ENOSYS;
    struct thread *t=thread_current();
    if (!t || p->state!=PROC_RUNNING || t->proc!=p || t->id!=p->tid ||
        current_root()!=p->root) return -VOS_EACCES;
    struct authority *a=authority(p);
    if (!a || !worker_id || stopping) return -VOS_EACCES;
    if (p->kill_reason) return -VOS_EINTR;
    if (f->rax==SYS_APP_NEXT) {
        if (!(a->rights & EXEC_ENUM)) return -VOS_EACCES;
        if (f->rdx!=sizeof(struct vos_app_entry_v1)) return -VOS_EINVAL;
        unsigned index=0;
        if (f->rdi) {
            if ((f->rdi>>4)!=a->generation) return -VOS_ESTALE;
            unsigned low=(unsigned)(f->rdi & 15);
            if (!low || low>catalog->count+1) return -VOS_EINVAL;
            index=low-1;
        }
        /* Validate even end-of-enumeration calls; no output changes on errors. */
        if (user_writable(p->root,f->rsi,sizeof(struct vos_app_entry_v1))) return -VOS_EFAULT;
        while (index<catalog->count) {
            const struct app_catalog_entry *e=app_catalog_at(catalog,index++);
            if (!listed(a,e->identity)) continue;
            struct vos_app_entry_v1 out={.version=1,.size=48,.app_id=e->identity,
                .next_cookie=(a->generation<<4)|(index+1),.name_length=(uint32_t)strlen(e->name)};
            memcpy(out.name,e->name,sizeof(out.name));
            if (copy_to_user(p->root,f->rsi,&out,sizeof(out))) return -VOS_EFAULT;
            return sizeof(out);
        }
        return 0;
    }
    if (f->rax!=SYS_APP_RUN_FOREGROUND) return -VOS_ENOSYS;
    if (!(a->rights & EXEC_RUN)) return -VOS_EACCES;
    if (!f->rdi || f->rsi) return -VOS_EINVAL;
    if (!listed(a,f->rdi)) return -VOS_EACCES;
    const struct app_catalog_entry *e=app_catalog_find(catalog,f->rdi);
    if (!e || e->policy!=APP_POLICY_DIAGNOSTIC) return -VOS_EUNSUPP;
    if (job.state!=JOB_FREE) return -VOS_EAGAIN;
    memset(&job,0,sizeof(job));
    job.parent=p;job.pid=p->pid;job.tid=p->tid;
    job.generation=a->generation;job.app_id=f->rdi;job.state=JOB_QUEUED;
    statistics.submitted++;
    event(EXEC_EV_QUEUED);
    thread_wake_all(&requests);
    while (job.state!=JOB_DONE) {
        if (p->kill_reason) exec_cancel_request(p);
        thread_block(&job.completion);
    }
    int64_t result=job.result;
    memset(&job,0,sizeof(job)); /* Only after child destruction and join. */
    return result;
}
