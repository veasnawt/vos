/* Gate B only: deterministic SC01/SC02, existing authoritative observations. */
#include "storage_stress.h"
#include "storage_observe.h"
#include "console.h"
#include "exception.h"
#include "irq.h"
#include "lib.h"
#include "loader.h"
#include "memlayout.h"
#include "panic.h"
#include "pit.h"
#include "pmm.h"
#include "syscall.h"
#include "usercopy.h"

static struct process procs[PROC_TABLE];
static struct storage_observation sample;
static uint64_t allocation[PMM_WORDS(PMM_MAX_FRAMES)];
static struct {
    struct observe_thread threads[THREAD_MAX];
    uint64_t cache[BLK_CACHE_LINES], pool[VBLK_POOL_FRAMES], free, usable;
    unsigned devices;
} baseline;
static struct wait_queue progress_gate;
static unsigned checks, current_group, cpu_phase, cpu_release, phase;
static unsigned victim_slot, grant_index, term_class;
static uint64_t buffer_address[2], old_token, foreign_token, victim_buffer;
static uint64_t prior_pid[PROC_TABLE], prior_tid[PROC_TABLE], reads[2];
static const uint8_t exit_code[]={0x31,0xc0,0x31,0xff,0xcd,0x80,0x0f,0x0b};
static struct { unsigned read_ok,read_bad,open_ok,open_full,stat_ok,next_ok,close_ok,initial_ok,initial_bad; } calls;
_Static_assert(sizeof(procs)+sizeof(sample)+sizeof(allocation)+sizeof(baseline)+
               sizeof(struct stress_trace)+1024 <=96*1024,"Gate B complete bounded static subset");

static int same(const char *a,const char *b) { return strlen(a)==strlen(b) && !memcmp(a,b,strlen(a)); }
static void require(int ok,const char *why) {
    checks++;
    if(!ok) {
        serial_puts("storage-stress: FAIL: ");serial_puts(why);serial_puts("\r\n");
        panic("Gate B failed; architectural review required");
    }
}
static void capture(void) { require(!storage_observe(&sample),"IF=0 pure observation"); }
static void trace_reset(void) {
    require(!io_scope_count(),"trace reset only at scope quiescence");
    memset(&storage_stress_trace,0,sizeof(storage_stress_trace));
}
static const struct boot_module *module(const struct boot_info *bi,const char *name) {
    for(unsigned i=0;i<bi->module_count;i++)
        if(strlen(bi->modules[i].cmdline)==strlen(name) &&
           !memcmp(bi->modules[i].cmdline,name,strlen(name))) return &bi->modules[i];
    return NULL;
}
static void load(const struct boot_info *bi,unsigned pos,const char *name,unsigned abi) {
    const struct boot_module *m=module(bi,name);struct elf_image image;int error;
    require(m && !load_elf(&procs[pos],name,phys_to_virt(m->start),m->end-m->start,&image,&error),
            "independent ELF module load");
    require(procs[pos].abi==abi,"exact executable ABI note");
}
static void snapshot_baseline(void) {
    capture();
    memcpy(baseline.threads,sample.threads,sizeof(baseline.threads));
    for(unsigned i=0;i<BLK_CACHE_LINES;i++) baseline.cache[i]=sample.block.lines[i].frame;
    memcpy(baseline.pool,sample.driver.pool,sizeof(baseline.pool));
    baseline.free=sample.pmm_free;baseline.usable=sample.pmm_usable;
    baseline.devices=sample.block.devices_count;
    memcpy(allocation,pmm_kernel()->alloc,sizeof(allocation));
}
static void quiet(void) {
    capture();
    require(!sample.file.fs && !sample.file.refs && !sample.file.active && !sample.file.config_owner &&
            !file_session_count() && !file_handle_count() && !channel_count(),"no file/channel/session owner");
    for(unsigned i=0;i<PROC_TABLE;i++) {
        require(!sample.processes[i].record && !sample.file.sessions[i].owner,"no process or session identity");
        for(unsigned j=0;j<FILE_MAX;j++) require(!sample.file.sessions[i].nodes[j].used,"no reserved/published node");
    }
    for(unsigned i=0;i<THREAD_MAX;i++) {
        const struct observe_thread *a=&baseline.threads[i],*b=&sample.threads[i];
        require(a->record==b->record && a->tid==b->tid && a->process==b->process &&
                a->lo==b->lo && a->hi==b->hi && a->stack_owner==b->stack_owner &&
                a->stack==b->stack,"exact baseline thread/stack identity");
    }
    for(unsigned i=0;i<EXT2_MAX_FS;i++)
        require(!sample.readers[i].used && !sample.readers[i].busy && !sample.readers[i].head &&
                !sample.readers[i].tail,"no reader/wait owner");
    for(unsigned i=0;i<IO_SCOPE_TICKETS;i++) require(!sample.tickets[i].id,"no ticket/debt");
    require(sample.block.devices_count==baseline.devices && !sample.block.head && !sample.block.tail,
            "device registry/cache wait baseline");
    for(unsigned i=0;i<BLK_CACHE_LINES;i++)
        require(sample.block.lines[i].frame==baseline.cache[i] && !sample.block.lines[i].pins &&
                sample.block.lines[i].state!=BLK_LINE_LOADING,"cache identities/pins/loaders");
    require(sample.driver.state==VBLK_READY && sample.driver.pool_held && !sample.driver.quarantined &&
            sample.driver.descriptors_available,"healthy retained DMA baseline");
    for(unsigned i=0;i<VBLK_POOL_FRAMES;i++) require(sample.driver.pool[i]==baseline.pool[i],"DMA frame identity");
    for(unsigned i=0;i<VBLK_SLOTS;i++) {
        const struct observe_slot *s=&sample.driver.slots[i];
        require(!s->owner && !s->tid && !s->ticket && !s->inflight && !s->done && !s->abandoned &&
                !s->head && !s->tail,"no surviving driver owner");
    }
    require(sample.pmm_free==baseline.free && sample.pmm_usable==baseline.usable &&
            !memcmp(allocation,pmm_kernel()->alloc,sizeof(allocation)) && !pmm_check_in(pmm_kernel()),
            "exact allocation bitmap restored");
    require(!proc_audit() && !sched_audit() && !channel_audit() && !file_audit() &&
            !ext2_audit() && !blk_audit() && !vblk_audit() && !io_scope_audit(),"all authoritative audits");
}
static void attach(void) {
    require(!proc_create(&procs[0],"gb-setup",exit_code,sizeof(exit_code)),"temporary registered setup");
    require(!file_volume_attach(&procs[0],vblk_info()->blk_id),"real read-only transport attach");
    proc_destroy(&procs[0]);
}
static void identities(unsigned pos,int has_thread) {
    capture();
    struct process *p=&procs[pos];
    const struct observe_process *o=&sample.processes[pos];
    require(o->record==(uint64_t)p && o->pid==p->pid && o->root==p->root &&
            o->tid==p->tid && o->state==(unsigned)p->state && o->pages==p->npages &&
            o->stack_top==p->kstack_top,"actual intended registered process position and incarnation");
    unsigned matches=0;
    for(unsigned i=0;i<THREAD_MAX;i++) if(sample.threads[i].process==(uint64_t)p) {
        const struct observe_thread *t=&sample.threads[i];matches++;
        require(has_thread && t->record==(uint64_t)thread_test_get(p->tid) && t->tid==p->tid &&
                t->stack_owner==t->record && t->hi==p->kstack_top && t->lo<t->rsp && t->rsp<t->hi,
                "real thread record/TID/stack owner and bounds");
    }
    require(matches==(unsigned)has_thread,"exact caller thread multiplicity");
}
static void grants(unsigned pos,const char *path) {
    const struct file_grant g={0,FILE_RIGHTS,path,strlen(path)};
    require(!file_grants_install(&procs[pos],&g,1),"explicit directory-scoped grant");
}
/*
 * Controller pacing while user threads or the device make progress. Polling transport: yield, as before
 * (the waiting reader polls while runnable and no time passes), at most PACE_ROUNDS rounds. Interrupt
 * transport: the kernel runs with IF = 0 and takes interrupts only in the idle thread, which never runs
 * while the controller yields to itself (rerun-2: the leader's completion could never arrive), so each
 * round sleeps one tick instead, until PACE_TICKS have passed (the selftest_file.c pattern). The
 * pre-notify loops in sc01() keep yielding: no tick passes while a request is deliberately held.
 */
#define PACE_ROUNDS 4096u
#define PACE_TICKS  (4u*VBLK_WAIT_TICKS)
static uint64_t pace_deadline(void) { return irq_ticks()+PACE_TICKS; }
static int pacing(unsigned round,uint64_t deadline) {
    return vblk_info()->mode==VBLK_MODE_IRQ ? irq_ticks()<deadline : round<PACE_ROUNDS;
}
static void pace(void) {
    if(vblk_info()->mode==VBLK_MODE_IRQ) thread_sleep(1); else thread_yield();
}
static void wait_blocked(unsigned pos) {
    uint64_t deadline=pace_deadline();
    for(unsigned i=0;pacing(i,deadline) && thread_state(procs[pos].tid)!=THREAD_BLOCKED;i++) pace();
    require(thread_state(procs[pos].tid)==THREAD_BLOCKED,"deterministic channel/progress barrier reached");
}
static void zero_send(unsigned pos,int64_t h) {
    require(!channel_send(&procs[pos],h,0,0),"existing channel command");
}
static void zero_ack(unsigned pos,int64_t h) {
    int64_t ret=-VOS_EAGAIN;uint64_t deadline=pace_deadline();
    for(unsigned i=0;pacing(i,deadline) && ret==-VOS_EAGAIN;i++) {
        ret=channel_recv(&procs[pos],h,0,0,0);
        if(ret==-VOS_EAGAIN) pace();
    }
    require(ret==0,"exact paced zero-length acknowledgment");
}
static void wait_progress(unsigned want,int cpu) {
    uint64_t deadline=pace_deadline();
    for(unsigned i=0;pacing(i,deadline) && (cpu?cpu_phase:phase)<want;i++) pace();
    require((cpu?cpu_phase:phase)==want,"exact progress phase reached, finite work bound");
}
/*
 * The CPU companion is released every 17 epochs, four quarters of SC01's 68. Each wait for a release may
 * last at most one quarter's share of the runner's 240-second group watchdog: 240 s x 17/68 = 60 s, at
 * PIT_HZ = 6000 ticks. The four windows together equal the group watchdog; a longer quarter fails.
 */
#define CPU_RELEASE_TICKS (240u*PIT_HZ*17u/68u)
_Static_assert(CPU_RELEASE_TICKS*4u==240u*PIT_HZ,"release windows partition the group watchdog");
void storage_stress_b_progress(struct process *p,const struct exc_frame *f,int64_t result) {
    if(current_group==1) {
        for(unsigned i=0;i<2;i++) if(p==&procs[i]) {
            if(f->rax==SYS_DEBUG_WRITE) {
                require(!buffer_address[i] && !result && !f->rsi && !user_writable(p->root,f->rdi,4096),
                        "one validated numeric reader buffer registration");
                buffer_address[i]=f->rdi;
            }
            if(f->rax==SYS_FILE_READ_AT) {
                require(result==4096,"every SC01 read returns exactly 4096");reads[i]++;
            }
        }
        if(p==&procs[2] && f->rax==SYS_DEBUG_WRITE) {
            require(!result && !f->rsi,"ABI v0 zero-length progress call");
            cpu_phase++;
            while(cpu_release<cpu_phase) {
                int e=thread_block_until(&progress_gate,irq_ticks()+CPU_RELEASE_TICKS);
                require(e==THREAD_WOKEN,"CPU deterministic release, no timeout");
            }
        }
    } else if(current_group==2 && p==&procs[victim_slot]) {
        switch(f->rax) {
        case SYS_FILE_READ_AT:
            if(result==4096) calls.read_ok++; else {require(result==-VOS_EBADHANDLE,"exact stale/foreign error");calls.read_bad++;}break;
        case SYS_FILE_OPEN_AT:
            if(result>0) calls.open_ok++; else {require(result==-VOS_ENOMEM,"exact exhausted handle error");calls.open_full++;}break;
        case SYS_FILE_STAT: require(!result,"stat result");calls.stat_ok++;break;
        case SYS_DIRECTORY_NEXT: require(result==1,"enumeration result");calls.next_ok++;break;
        case SYS_HANDLE_CLOSE: require(!result,"close result");calls.close_ok++;break;
        case SYS_FILE_INITIAL:
            if(f->rdi==2) {require(result==-VOS_EINVAL,"invalid initial index");calls.initial_bad++;}
            else {require(f->rdi==grant_index && result>0,"selected actual grant index");calls.initial_ok++;}break;
        case SYS_DEBUG_WRITE:
            require(!result && !f->rsi,"existing zero-length debug setup/progress");
            phase++;
            if(phase==1) {
                struct {uint64_t index,term,old,foreign,buffer;} command;
                require(!copy_from_user(p->root,&command,f->rdi,sizeof(command)),"validated test configuration record");
                command.index=grant_index;command.term=term_class;command.old=old_token;command.foreign=foreign_token;
                victim_buffer=command.buffer;
                require(!user_writable(p->root,victim_buffer,4096) &&
                        !copy_to_user(p->root,f->rdi,&command,sizeof(command)),"trusted scalar configuration only");
            } else {
                require(phase==2,"one final lifecycle barrier");
                while(phase==2 && !p->kill_reason) {
                    int e=thread_block_until(&progress_gate,irq_ticks()+100);
                    require(e==THREAD_WOKEN || (e==THREAD_INTERRUPTED && p->kill_reason),
                            "lifecycle release or remembered authorized kill");
                }
            }
            break;
        default:break;
        }
    }
}
static void buffer_sentinel(unsigned pos) {
    uint8_t bytes[128];
    require(buffer_address[pos] && procs[pos].state==PROC_RUNNING,"live registered buffer owner");
    for(unsigned j=0;j<4096;j+=sizeof(bytes)) {
        require(!copy_from_user(procs[pos].root,bytes,buffer_address[pos]+j,sizeof(bytes)),"revalidate every observed user range");
        for(unsigned k=0;k<sizeof(bytes);k++) require(bytes[k]==0xa5,"no premature publication of any reader byte");
    }
}
static void overlap(unsigned lead,unsigned follow) {
    identities(lead,1);identities(follow,1);capture();
    uint64_t ticket[2]={0,0};unsigned tickets=0,requests=0,busy=0,sessions=0;
    for(unsigned i=0;i<IO_SCOPE_TICKETS;i++) if(sample.tickets[i].id) {
        const struct observe_ticket *t=&sample.tickets[i];tickets++;
        unsigned owner=t->pid==procs[lead].pid?lead:follow;
        require(t->pid==procs[owner].pid && t->tid==procs[owner].tid && t->scope && t->guard &&
                !t->abandoned && t->request==(unsigned)(owner==lead) && !t->result &&
                t->logical<=512 && t->reads<=256 && t->waits<=1024,"two distinct live bounded ticket owners");
        ticket[owner]=t->id;
    }
    require(tickets==2 && ticket[0] && ticket[1] && ticket[0]!=ticket[1],"exact two-ticket admission");
    for(unsigned i=0;i<PROC_TABLE;i++) if(sample.file.sessions[i].owner) {
        const struct observe_session *s=&sample.file.sessions[i];
        unsigned role=s->owner==(uint64_t)&procs[0]?0:1;unsigned nodes=0;sessions++;
        require(s->owner==(uint64_t)&procs[role] && s->pid==procs[role].pid &&
                s->tid==procs[role].tid && s->active && !s->closing && s->provisioned &&
                s->initial[0] && !s->initial[1],"live exact reader/grant/session owner");
        for(unsigned j=0;j<FILE_MAX;j++) {
            const struct observe_node *n=&s->nodes[j];
            require(n->used!=1,"no unexpected reservation");
            if(n->used==2) {nodes++;require(n->generation==sample.file.generation &&
                n->token>FILE_TOKEN_DOMAIN && (n->rights==FILE_RIGHTS || n->rights==(FILE_READ|FILE_STAT)),
                "nonstale attenuated file node authority");}
        }
        require(nodes==2,"exact initial grant and derived read handle");
    }
    require(sessions==2 && sample.file.active==2,"both real operations pending together");
    struct thread *f=thread_test_get(procs[follow].tid);
    for(unsigned i=0;i<EXT2_MAX_FS;i++) if(sample.readers[i].busy) {
        const struct observe_reader *reader=&sample.readers[i];busy++;
        require(reader->record==sample.file.fs && reader->head==(uint64_t)f &&
                reader->tail==(uint64_t)f && f && f->waiting_on &&
                f->state==THREAD_BLOCKED,
                "follower queued at actual shared ext2 reader");
    }
    require(busy==1 && thread_state(procs[follow].tid)==THREAD_BLOCKED,"genuine serialized reader wait");
    for(unsigned i=0;i<VBLK_SLOTS;i++) if(sample.driver.slots[i].inflight) {
        const struct observe_slot *s=&sample.driver.slots[i];requests++;
        require(s->owner==(uint64_t)thread_test_get(procs[lead].tid) && s->tid==procs[lead].tid &&
                s->ticket==ticket[lead] && !s->abandoned,"real pending leader transport request");
        for(unsigned j=i*3;j<i*3+3;j++)
            require(vblk_dma_ok(sample.driver.descriptors[j].address,sample.driver.descriptors[j].length),
                    "driver-owned descriptors remain within retained DMA pool");
    }
    require(requests==1,"no fabricated or duplicate completion");
    buffer_sentinel(0);buffer_sentinel(1);
    require(!file_audit() && !proc_audit() && !sched_audit() && !io_scope_audit(),"active ownership invariants");
}
static void trace_read_epoch(uint64_t submitted,uint64_t completed) {
    const struct stress_trace *t=&storage_stress_trace;
    require(!t->overflow,"complete per-epoch bounded trace");
    require(t->kinds[STRESS_COMPLETE] && t->kinds[STRESS_COMPLETE]==t->kinds[STRESS_RETIRE] &&
            t->kinds[STRESS_RETIRE]==vblk_info()->submitted-submitted &&
            t->kinds[STRESS_RETIRE]==vblk_info()->completed-completed,"exact real request completion/retirement");
    unsigned scopes[2]={0,0};
    for(unsigned i=0;i<t->count;i++) {
        const struct stress_event *e=&t->events[i];
        require(e->sequence==i+1,"exact event ordering");
        if(e->kind==STRESS_SCOPE_END) {
            unsigned role=e->identity==procs[0].pid?0:1;
            require(e->identity==procs[role].pid && e->tid==procs[role].tid && e->ticket && !e->result,
                    "read scope retires for exact PID/TID/ticket");scopes[role]++;
        }
        if(e->kind==STRESS_COMPLETE || e->kind==STRESS_RETIRE || e->kind==STRESS_POLL)
            require((e->tid==procs[0].tid || e->tid==procs[1].tid) && e->ticket && !e->result,
                    "transport trace tied only to actual reader ticket");
    }
    require(scopes[0]==1 && scopes[1]==1 && !io_scope_count(),"both read scopes retired exactly once");
}
static void sc01(const struct boot_info *bi) {
    serial_puts("storage-stress: SC01 BEGIN seed=0x4E360001 epochs=68 reads=136\r\n");
    current_group=1;cpu_phase=cpu_release=0;reads[0]=reads[1]=0;buffer_address[0]=buffer_address[1]=0;
    attach();
    load(bi,0,"stress-r0",2);load(bi,1,"stress-r1",2);load(bi,2,"stress-cpu",0);
    load(bi,3,"stress-ipc0",1);load(bi,4,"stress-ipc1",1);
    require(!proc_create(&procs[5],"gb-control",exit_code,sizeof(exit_code)),"sixth process is READY kernel controller");
    grants(0,"scope/r0");grants(1,"scope/r1");
    int64_t reader[2],ipch,own,peer;
    for(unsigned i=0;i<2;i++) {
        require(!channel_create(&procs[i],HANDLE_RIGHTS,&procs[5],HANDLE_RIGHTS,&own,&reader[i]) && own==1,
                "reader/controller channel handles");
    }
    require(!channel_create(&procs[3],HANDLE_RIGHTS,&procs[5],HANDLE_RIGHTS,&own,&ipch) && own==1,
            "IPC initiator pacing channel");
    require(!channel_create(&procs[3],HANDLE_RIGHTS,&procs[4],HANDLE_RIGHTS,&own,&peer) && own==2 && peer==1,
            "independent ABI v1 IPC pair");
    for(unsigned i=0;i<5;i++) require(!proc_start(&procs[i]),"start exact independent user processes");
    for(unsigned i=0;i<5;i++) wait_blocked(i);
    require(proc_count()==6 && cpu_phase==1,"six-process bound and initial CPU barrier");
    unsigned visits[17]={0};
    for(unsigned epoch=0;epoch<68;epoch++) {
        unsigned lead=epoch&1,follow=lead^1;
        for(unsigned i=0;i<2;i++) wait_blocked(i);
        require(!blk_invalidate(vblk_info()->blk_id),"cold data path without live scope");
        trace_reset();
        uint64_t submitted=vblk_info()->submitted,completed=vblk_info()->completed;
        vblk_test_skip_notify(1);vblk_test_yield_polls(32);
        zero_send(5,reader[lead]);
        unsigned pending=0;
        for(unsigned i=0;i<4096;i++) {
            capture();pending=0;
            for(unsigned j=0;j<VBLK_SLOTS;j++) pending+=sample.driver.slots[j].inflight &&
                sample.driver.slots[j].tid==procs[lead].tid;
            if(pending) break;
            thread_yield();
        }
        require(pending==1 && file_process_busy(&procs[lead]) && !file_process_busy(&procs[follow]),
                "leader pending before follower released");
        zero_send(5,reader[follow]);
        for(unsigned i=0;i<4096 && !file_process_busy(&procs[follow]);i++) thread_yield();
        require(file_process_busy(&procs[follow]),"follower entered file API while leader pending");
        overlap(lead,follow);
        vblk_test_notify();
        zero_ack(5,reader[lead]);zero_ack(5,reader[follow]);
        if(epoch==67) {
            for(unsigned i=0;i<2;i++) require(proc_wait(&procs[i])==PROC_EXITED && !procs[i].exit_status,
                    "both readers verified every complete independent pattern");
        } else for(unsigned i=0;i<2;i++) wait_blocked(i);
        require(reads[0]==epoch+1 && reads[1]==epoch+1,"exact successful read total per epoch");
        trace_read_epoch(submitted,completed);
        zero_send(5,ipch);zero_ack(5,ipch);
        if(epoch==67) {
            require(proc_wait(&procs[3])==PROC_EXITED && !procs[3].exit_status &&
                    proc_wait(&procs[4])==PROC_EXITED && !procs[4].exit_status,"68 exact ABI v1 round trips");
        } else wait_blocked(3);
        if(epoch==16 || epoch==33 || epoch==50 || epoch==67) {
            unsigned checkpoint=(epoch+1)/17;
            require(cpu_phase==checkpoint,"CPU paced at approved quarter boundary");
            cpu_release=checkpoint;thread_wake_all(&progress_gate);wait_progress(checkpoint+1,1);
            serial_puts("storage-stress: SC01 CPU epoch=");serial_dec(epoch);serial_puts(" PASS\r\n");
        }
        visits[(5*epoch)%17]++;
        serial_puts("storage-stress: SC01 EPOCH ");serial_dec(epoch);serial_puts(" region=");
        serial_dec((5*epoch)%17);serial_puts(" lead=");serial_dec(lead);serial_puts(" reads=2 overlap=1 accounting=PASS\r\n");
    }
    for(unsigned i=0;i<17;i++) require(visits[i]==4,"four independent visits per region per reader");
    require(reads[0]==68 && reads[1]==68 && procs[3].ipc_in==136 && procs[3].ipc_out==136 &&
            procs[4].ipc_in==68 && procs[4].ipc_out==68,"136 reads plus 68 independent IPC round trips");
    cpu_release=5;thread_wake_all(&progress_gate);
    require(proc_wait(&procs[2])==PROC_EXITED && !procs[2].exit_status && procs[2].abi==0 &&
            procs[2].syscalls==5,"ABI v0 CPU state and progress preserved");
    for(unsigned i=0;i<6;i++) proc_destroy(&procs[i]);
    vblk_test_yield_polls(VBLK_YIELD_POLLS);
    require(!file_volume_detach(),"SC01 volume detach");quiet();current_group=0;
    serial_puts("storage-stress: SC01 PASS epochs=68 reads=136 overlaps=68 cpu=4 ipc=68 accounting=PASS\r\n");
}
static void check_no_victim_grant(void) {
    capture();
    for(unsigned i=0;i<PROC_TABLE;i++) require(sample.file.sessions[i].owner!=(uint64_t)&procs[victim_slot],
                                               "failed setup publishes no victim grant/session");
    require(file_handle_count()==1 && file_session_count()==1 && !sample.file.active && sample.file.refs==1 &&
            !io_scope_count() && !file_audit(),"setup failure leaves only explicit foreign authority");
}
static void setup_case(const struct boot_info *bi,unsigned pos,unsigned index,unsigned term) {
    victim_slot=pos;grant_index=index;term_class=term;phase=0;memset(&calls,0,sizeof(calls));
    for(unsigned i=0;i<PROC_TABLE;i++) {
        if(i==pos && term!=3) load(bi,i,"stress-life",2);
        else require(!proc_create(&procs[i],i==pos?"gb-ready":"gb-placeholder",exit_code,sizeof(exit_code)),
                     "real registered READY process position");
        require(procs[i].pid>prior_pid[i],"nonreused PID in actual reused process record");
        prior_pid[i]=procs[i].pid;identities(i,0);
    }
    require(proc_count()==8,"all eight real process-record positions occupied, no ninth controller");
    unsigned other=(pos+1)%8;grants(other,"scope");foreign_token=file_initial(&procs[other],0);
    require((int64_t)foreign_token>0,"live foreign-process token");
    struct process *p=&procs[pos];
    const struct file_grant missing[2]={{0,FILE_RIGHTS,"scope",5},{1,FILE_RIGHTS,"scope/not-present",17}};
    require(file_grants_install(p,missing,2)==-FILE_ENOENT,"missing second target retains actual lookup error");
    check_no_victim_grant();
    struct file_grant invalid={2,FILE_RIGHTS,"scope",5};
    require(file_grants_install(p,&invalid,1)==-VOS_EINVAL,"invalid full-width grant index");
    check_no_victim_grant();
    require(file_grants_install(p,missing,3)==-VOS_EINVAL,"invalid grant count before input access");
    check_no_victim_grant();
    const struct file_grant valid[2]={{0,FILE_RIGHTS,"scope",5},{1,FILE_RIGHTS,"scope",5}};
    require(!file_grants_install(p,valid,2),"atomic two-grant setup");
    require(file_initial(p,2)==-VOS_EINVAL,"invalid initial index");
    uint64_t fresh=file_initial(p,index);
    require((int64_t)fresh>0 && fresh!=old_token && fresh!=foreign_token,"fresh token/incarnation and authority");
    require(file_read_at(p,old_token,&sample,4096,0)==-VOS_EBADHANDLE &&
            file_read_at(p,foreign_token,&sample,4096,0)==-VOS_EBADHANDLE,"stale/foreign tokens confer no authority");
    trace_reset();
}
static void native_case(struct process *p,unsigned index) {
    int64_t base=file_initial(p,index),f=file_open_at(p,base,"r0/data.bin",11,FILE_READ|FILE_STAT);
    require(base>0 && f>0 && file_read_at(p,f,&sample,4096,0)==4096,"native READY real file open/read");
    const uint8_t *bytes=(const uint8_t *)&sample;
    for(unsigned j=0;j<4096;j++) require(bytes[j]==(uint8_t)(((0x4e360001U>>(8*(j&3)))+17*j+(j>>8))&255),
                                             "native full independent expected data");
    struct ext2_stat st;require(!file_stat(p,f,&st) && st.size==69632,"native metadata query");
    int64_t d=file_open_at(p,base,"canary",6,FILE_ENUMERATE|FILE_STAT);uint64_t cookie=0;struct ext2_dirent ent;
    require(d>0 && file_next(p,d,&cookie,&ent)==1 && cookie==1024,"native directory enumeration");
    require(!file_close(p,f) && !file_close(p,d) &&
            file_read_at(p,f,&sample,4096,0)==-VOS_EBADHANDLE,"native close and stale generation");
    int64_t h[6];
    for(unsigned i=0;i<6;i++) require((h[i]=file_open_at(p,base,".",1,FILE_STAT))>0,"all remaining handle slots");
    require(file_open_at(p,base,".",1,FILE_STAT)==-VOS_ENOMEM,"exact eight-handle exhaustion");
    for(unsigned i=0;i<6;i++) require(!file_close(p,h[i]),"reclaim exhausted handle slots");
    require(file_open_at(p,base,"r0/data.bin",11,FILE_READ)>0 &&
            file_open_at(p,base,"canary",6,FILE_ENUMERATE)>0,"live derived nodes for never-started destruction");
    identities(victim_slot,0);require(p->state==PROC_READY && !p->tid,"never started; no invented user execution");
}
static void sc02(const struct boot_info *bi) {
    serial_puts("storage-stress: SC02 BEGIN seed=0x4E360002 cases=64\r\n");current_group=2;attach();
    unsigned total=0,classes[4]={0,0,0,0};
    for(unsigned pos=0;pos<8;pos++) for(unsigned index=0;index<2;index++) for(unsigned term=0;term<4;term++) {
        setup_case(bi,pos,index,term);struct process *p=&procs[pos];uint64_t issued=file_initial(p,index);
        if(term==3) native_case(p,index);
        else {
            require(!proc_start(p) && p->tid>prior_tid[pos],"new user thread incarnation");
            prior_tid[pos]=p->tid;wait_progress(2,0);identities(pos,1);
            require(calls.read_ok==1 && calls.read_bad==3 && calls.open_ok==10 && calls.open_full==1 &&
                    calls.stat_ok==1 && calls.next_ok==1 && calls.close_ok==8 &&
                    calls.initial_ok==1 && calls.initial_bad==1,"exact user API/error/exhaustion operation matrix");
            require(file_process_busy(p)==0 && !io_scope_count(),"final barrier holds no storage scope");
        }
        capture();unsigned nodes=0;
        for(unsigned i=0;i<PROC_TABLE;i++) if(sample.file.sessions[i].owner==(uint64_t)p) {
            const struct observe_session *s=&sample.file.sessions[i];
            require(s->pid==p->pid && !s->active && !s->tid && s->initial[0] && s->initial[1] &&
                    s->initial[0]!=s->initial[1],"two exact surviving grants owned by current incarnation");
            for(unsigned j=0;j<FILE_MAX;j++) {
                if(s->nodes[j].used==2) {
                    require(s->nodes[j].generation==sample.file.generation,"current volume generation");nodes++;
                } else require(!s->nodes[j].used,"no hidden reservation");
            }
        }
        require(nodes==4 && !storage_stress_trace.overflow,"four real victim nodes before teardown; bounded complete trace");
        if(term==2) proc_kill(p,"Gate B authorized kill");
        if(term!=3) {
            if(term!=2) {phase=3;thread_wake_all(&progress_gate);}
            int state=proc_wait(p);
            require(state==(term==0?PROC_EXITED:PROC_KILLED),"exact exit/fault/kill state");
            if(term==0) require(!p->exit_status,"normal user exit zero");
            if(term==1) require(p->trap.vector==6,"actual user-mode invalid-opcode fault");
            if(term==2) require(p->reason && same(p->reason,"Gate B authorized kill") &&
                               p->trap.rax==SYS_DEBUG_WRITE,"authorized kill at held kernel boundary, no user return");
            require(thread_state(p->tid)==THREAD_FREE && !file_process_busy(p),"dead caller has no live thread/scope");
        }
        proc_destroy(p);old_token=issued;
        for(unsigned i=0;i<8;i++) if(i!=pos) proc_destroy(&procs[i]);
        require(!file_volume_detach(),"per-case reader detach");quiet();
        total++;classes[term]++;
        serial_puts("storage-stress: SC02 CASE ");serial_dec(total-1);serial_puts(" slot=");serial_dec(pos);
        serial_puts(" grant=");serial_dec(index);serial_puts(" term=");serial_dec(term);
        serial_puts(" errors=PASS incarnation=PASS accounting=PASS\r\n");
        if(total<64) attach();
    }
    require(total==64 && classes[0]==16 && classes[1]==16 && classes[2]==16 && classes[3]==16,
            "exact 64-case matrix: 48 user executions plus 16 registered never-started native cases");
    current_group=0;
    serial_puts("storage-stress: SC02 PASS cases=64 user=48 ready=16 slots=8 grants=2 accounting=PASS\r\n");
}
int selftest_storage_stress_b(const struct boot_info *bi) {
    /* Fixed-order selector rejects unimplemented later groups, never labels them PASS. */
    const char *args=bi->cmdline?bi->cmdline:"";
    int run1=same(args,"n36-groups=SC01") || same(args,"n36-groups=SC01,SC02");
    int run2=same(args,"n36-groups=SC02") || same(args,"n36-groups=SC01,SC02");
    require(run1||run2,"explicit supported fixed-order group selection; SC03/04/05/10/11 unimplemented");
    const struct boot_module *m=module(bi,"storage-stress.img");
    require(m && m->end-m->start==8388608 && vblk_info()->capacity==16384 &&
            vblk_info()->state==VBLK_READY,"accepted immutable fixture and real ready transport");
    require(!proc_count() && !file_session_count() && !ext2_open_count() && !channel_count(),
            "existing prerequisite tests leave declared baseline");
    snapshot_baseline();quiet();
    serial_puts("storage-stress: TRANSPORT ");serial_puts(vblk_info()->mode==VBLK_MODE_IRQ?"IRQ":"POLL");
    serial_puts(" PASS\r\n");
    if(run1) sc01(bi);
    if(run2) sc02(bi);
    quiet();
    serial_puts("storage-stress: SC03 SC04 SC05 SC10 SC11 NOT RUN\r\n");
    serial_puts("storage-stress: CHECKS ");serial_dec(checks);serial_puts(" PASS\r\n");
    serial_puts("storage-stress: SELECTED PASS\r\n");
    return 0;
}
