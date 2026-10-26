/* Trusted boot-selected launch route. Negative controls are compiled only in
 * GUI_VERIFY builds; normal interactive applications have no verification deadline.
 */
#include "gui_verify.h"
#include "gui.h"
#include "app_payload.h"
#include "app_catalog.h"
#include "bootinfo.h"
#include "process.h"
#include "loader.h"
#include "pmm.h"
#include "thread.h"
#include "console.h"
#include "lib.h"
#include "syscall.h"
#include "exception.h"
static struct process application;
#ifdef VOS_GUI_VERIFY
static int bootstrap_testing,bootstrap_rejected;
#endif
static int option(const char *p,const char *token) {
    if(!p)return 0;
    while(*p) {
        while(*p==' ' || *p=='\t')p++;
        unsigned n=0;while(token[n] && p[n]==token[n])n++;
        if(!token[n] && (!p[n] || p[n]==' ' || p[n]=='\t'))return 1;
        while(*p && *p!=' ' && *p!='\t')p++;
    }
    return 0;
}
int gui_boot_requested(const char *p){return option(p,"vos.gui=1");}
int pointer_boot_requested(const char *p){return option(p,"vos.pointer=1");}
static int load(struct process *p,uint64_t identity) {
    const struct app_catalog_entry *e=app_catalog_find(app_payload_catalog(),identity);
    if(!e || e->policy!=APP_POLICY_GUI || e->abi!=5)return PROC_EEXEC;
    struct elf_image image;int error;
#ifdef VOS_GUI_VERIFY
    if(bootstrap_testing && identity==0x1004) {
        thread_yield(); /* Let the actual input worker block before rejection. */
        int result=load_elf(p,e->name,e->image,1,&image,&error);
        bootstrap_rejected=result==PROC_EEXEC && error==ELF_ESIZE;
        return result;
    }
#endif
    return load_elf(p,e->name,e->image,e->length,&image,&error);
}
#ifdef VOS_GUI_VERIFY
static int active,mode,armed,pointer_verify;
static struct process probe,fillers[PROC_TABLE];
int gui_verify_requested(const char *p){return option(p,"vos.gui-verify=1");}
int64_t gui_verify_syscall(struct process *p,const struct exc_frame *f) {
    if(!active || p!=&probe || !proc_file_live(p) || p->state!=PROC_RUNNING)return -VOS_EACCES;
    if(f->rsi || f->rdx)return -VOS_EINVAL;
    if(f->rdi==0)return mode;
    if(f->rdi==1 && (mode==6 || mode==7 || mode==13 || mode==14)){armed=1;return 0;}
    if(f->rdi==2 && (mode==8 || mode==12)){thread_sleep(300);return 0;}
    return -VOS_EINVAL;
}
void gui_verify_tick(struct process *p) {
    if(!active || !armed || p!=&probe || !gui_waiting(p))return;
    armed=0;
    if(mode==6 || mode==13)proc_kill(p,"GUI verification cancellation while blocked");
    else gui_revoke(p);
}
static int verify(void) {
    active=1;
    uint64_t free=pmm_free_count();unsigned threads=thread_count(),processes=proc_count();
    for(mode=0;mode<=(pointer_verify?15:9);mode++) {
        armed=0;
        if(load(&probe,mode==15?0x1004:mode>=10?0x1007:0x1005)!=PROC_OK)return 0;
        if(mode!=3 && gui_grant(&probe,GUI_ALL|(mode>=11 && mode<=14?GUI_POINTER:0)))return 0;
        if(mode==4)gui_test_alloc_budget(3);
        int state=proc_run(&probe);
        gui_test_alloc_budget(-1);
        int ok=mode==5 ? state==PROC_KILLED && probe.trap.vector==6 :
               (mode==6 || mode==13) ? state==PROC_KILLED && probe.kill_reason :
                         state==PROC_EXITED && probe.exit_status==0;
        proc_destroy(&probe);
        if(!ok || gui_frames() || pmm_free_count()!=free || thread_count()!=threads ||
           proc_count()!=processes || proc_audit() || sched_audit() || !thread_canaries_intact())return 0;
        serial_puts("gui-verify: case ");serial_dec(mode);serial_puts(" PASS; no leaked frames/processes/threads\r\n");
    }
    unsigned used=0;
    while(proc_count()<PROC_TABLE) {
        static const uint8_t code[]={0xc3};
        if(proc_create(&fillers[used],"gui-fill",code,sizeof(code))!=PROC_OK)return 0;
        used++;
    }
    int result=load(&probe,0x1005);
    while(used)proc_destroy(&fillers[--used]);
    if(result!=PROC_EBUSY || pmm_free_count()!=free || proc_count()!=processes)return 0;
    serial_puts("gui-verify: process exhaustion rollback PASS\r\n");
    active=0;return 1;
}

/* Exercise the real initial-load failure after its worker has run and blocked.
 * This exists only in the explicitly selected verification image. */
static int verify_bootstrap_failures(const struct boot_info *bi) {
    uint64_t free=pmm_free_count();
    unsigned processes=proc_count(),threads=thread_count();
    bootstrap_testing=1;
    for(unsigned i=0;i<THREAD_MAX+2;i++) {
        struct sched_stats before=*sched_stats();
        bootstrap_rejected=0;
        int result=gui_boot_run(bi);
        const struct sched_stats *after=sched_stats();
        if(result || !bootstrap_rejected || after->created!=before.created+1 ||
           after->reaped!=before.reaped+1 || after->blocks<=before.blocks ||
           gui_frames() || pmm_free_count()!=free || proc_count()!=processes ||
           thread_count()!=threads || proc_audit() || sched_audit() ||
           !thread_canaries_intact()) {
            bootstrap_testing=0;return 0;
        }
        serial_puts("gui-bootstrap: rejected ELF ");serial_dec(i);
        serial_puts(" PASS; worker blocked, awakened and reaped; resources restored\r\n");
    }
    bootstrap_testing=0;
    serial_puts("gui-bootstrap: 18 repeated load failures PASS; no retained workers\r\n");
    return 1;
}

#endif

int gui_boot_run(const struct boot_info *bi) {
#ifdef VOS_GUI_VERIFY
    if(!bootstrap_testing && gui_verify_requested(bi->cmdline) &&
       !verify_bootstrap_failures(bi))return 0;
#endif
    uint64_t free=pmm_free_count();unsigned processes=proc_count(),threads=thread_count();
    int pointer=pointer_boot_requested(bi->cmdline);
#ifdef VOS_GUI_VERIFY
    pointer_verify=pointer;
#endif
    int ok=0,ran=0;
    if(gui_service_start())return 0;
#ifdef VOS_GUI_VERIFY
    if(!bootstrap_testing && gui_verify_requested(bi->cmdline) && !verify()) {
        /* Verification can fail with a READY probe or allocated table fillers.
         * proc_run has joined any started probe before returning. */
        active=armed=0;gui_test_alloc_budget(-1);
        if(probe.pid)proc_destroy(&probe);
        for(unsigned i=0;i<PROC_TABLE;i++)if(fillers[i].pid)proc_destroy(&fillers[i]);
        goto stop;
    }
#else
    (void)bi;
#endif
    uint64_t identity=pointer?0x1006:0x1004;
#ifdef VOS_GUI_VERIFY
    if(bootstrap_testing)identity=0x1004;
#endif
    if(load(&application,identity)!=PROC_OK)goto stop;
    if(gui_grant(&application,GUI_ALL|(pointer?GUI_POINTER:0))) {proc_destroy(&application);goto stop;}
    serial_puts("gui: native ABI5 demo; one surface <=163840 bytes; upload <=4096 bytes; event queue=16\r\n");
    int state=proc_run(&application);
    ran=1;ok=state==PROC_EXITED && application.exit_status==0;
    proc_destroy(&application);
stop:
    /* The service owns its worker until stop wakes it and joins THREAD_FREE.
     * No process was published on loader failure; do not destroy it twice. */
    if(gui_service_stop())return 0;
    ok=ok && gui_frames()==0 && pmm_free_count()==free && proc_count()==processes &&
       thread_count()==threads && !proc_audit() && !sched_audit() && thread_canaries_intact();
    if(ran)serial_puts(ok ? "gui: PASS; kernel-CR3 reclamation, all resources restored\r\n" : "gui: FAIL\r\n");
    return ok;
}
