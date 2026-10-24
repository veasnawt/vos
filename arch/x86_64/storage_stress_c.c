/* Gate C only: deterministic SC03/SC04/SC05 over existing authoritative observations. */
#include "storage_stress.h"
#include "storage_observe.h"
#include "blockdev.h"
#include "console.h"
#include "exception.h"
#include "file.h"
#include "irq.h"
#include "lib.h"
#include "loader.h"
#include "memlayout.h"
#include "panic.h"
#include "pit.h"
#include "pmm.h"
#include "syscall.h"
#include "thread.h"
#include "usercopy.h"
#include "virtio_blk.h"

enum { OP_OPEN, OP_READ, OP_STAT, OP_ENUM, OP_READ_R1 };
enum { SITE_DEVICE, SITE_READER };
enum { KILL_FIRST, COMPLETE_FIRST };
enum { REUSE_SAME, REUSE_OTHER };
enum { FAULT_FALLBACK, FAULT_PREFIX, FAULT_EINTR };
enum { ROLE_OWNER, ROLE_WAITER };
#define V 0                 /* victim position */
#define H 1                 /* holder / second reader position */
#define O 2                 /* other reuse position */
/* Each wait for one case's progress may use at most one SC03 case's share of the 240-second group watchdog. */
#define CASE_TICKS (240u*PIT_HZ/32u)
#define PACE_ROUNDS 4096u
#define PACE_TICKS  (4u*VBLK_WAIT_TICKS)

static struct process procs[PROC_TABLE];
static struct storage_observation sample;
static uint64_t allocation[PMM_WORDS(PMM_MAX_FRAMES)];
static struct {
    struct observe_thread threads[THREAD_MAX];
    uint64_t cache[BLK_CACHE_LINES], pool[VBLK_POOL_FRAMES], free, usable;
    unsigned devices;
} baseline;
static struct victim {
    unsigned active, phase, release, results;
    uint64_t op, attempts, region, buffer, initial, stale;
    struct { uint64_t rax; int64_t res; } result[8];
} vic[PROC_TABLE];
static struct wait_queue barrier_q, hold_q, mock_q;
static unsigned checks, hold_state;       /* hold: 0 idle, 1 armed, 2 held, 3 passed */
static uint64_t hold_tid, prior_pid[PROC_TABLE];
static int hold_op_result, hold_wait, hold_killed;
static struct process *hold_proc;
static const uint8_t exit_code[]={0x31,0xc0,0x31,0xff,0xcd,0x80,0x0f,0x0b};
static const uint8_t *image;
static struct {
    uint64_t tid, hold_tid, multi_failed, single_ok, single_failed;
    int fault, armed, hold, held;
} md;
static uint8_t scratch[4096];
_Static_assert(sizeof(procs)+sizeof(sample)+sizeof(allocation)+sizeof(baseline)+sizeof(vic)+sizeof(scratch)+
               sizeof(struct stress_trace)+1024 <=96*1024,"Gate C complete bounded static subset");

static int same(const char *a,const char *b) { return strlen(a)==strlen(b) && !memcmp(a,b,strlen(a)); }
static void require(int ok,const char *why) {
    checks++;
    if(!ok) {
        serial_puts("storage-stress: FAIL: ");serial_puts(why);serial_puts("\r\n");
        panic("Gate C failed; architectural review required");
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

/* Controller pacing (the repaired Gate B rule): yield under polling, sleep a tick under interrupts. */
static uint64_t pace_deadline(void) { return irq_ticks()+PACE_TICKS; }
static int pacing(unsigned round,uint64_t deadline) {
    return vblk_info()->mode==VBLK_MODE_IRQ ? irq_ticks()<deadline : round<PACE_ROUNDS;
}
static void pace(void) {
    if(vblk_info()->mode==VBLK_MODE_IRQ) thread_sleep(1); else thread_yield();
}

static void attach(const struct boot_info *bi,blk_id_t device) {
    (void)bi;
    require(!proc_create(&procs[V],"gc-setup",exit_code,sizeof(exit_code)),"temporary registered setup");
    require(!file_volume_attach(&procs[V],device),"real read-only transport attach");
    proc_destroy(&procs[V]);
}
static void grants(unsigned pos) {
    const struct file_grant g={0,FILE_RIGHTS,"scope",5};
    require(!file_grants_install(&procs[pos],&g,1),"explicit directory-scoped grant");
}
/* Load the Gate C program at pos: one op, `attempts` attempts, each behind its own barrier. */
static void victim(const struct boot_info *bi,unsigned pos,uint64_t op,uint64_t attempts,uint64_t region,uint64_t stale) {
    const struct boot_module *m=module(bi,"stress-gatec");struct elf_image img;int error;
    require(m && !load_elf(&procs[pos],"stress-gatec",phys_to_virt(m->start),m->end-m->start,&img,&error) &&
            procs[pos].abi==2,"independent ABI v2 Gate C program");
    require(procs[pos].pid>prior_pid[pos],"nonreused PID in reused process record");
    prior_pid[pos]=procs[pos].pid;
    memset(&vic[pos],0,sizeof(vic[pos]));
    vic[pos]=(struct victim){.active=1,.op=op,.attempts=attempts,.region=region,.stale=stale};
    grants(pos);
    require(!proc_start(&procs[pos]),"start Gate C program");
}
/* Barrier k: the program issued its k-th zero-length debug write and is blocked there. */
static void wait_barrier(unsigned pos,unsigned k) {
    uint64_t deadline=pace_deadline();
    for(unsigned i=0;pacing(i,deadline) &&
        !(vic[pos].phase==k && thread_state(procs[pos].tid)==THREAD_BLOCKED);i++) pace();
    require(vic[pos].phase==k && thread_state(procs[pos].tid)==THREAD_BLOCKED,"exact Gate C barrier reached");
}
static void release(unsigned pos) { vic[pos].release=vic[pos].phase;thread_wake_all(&barrier_q); }
static int64_t last(unsigned pos,uint64_t rax) {
    for(unsigned i=vic[pos].results;i-->0;) if(vic[pos].result[i].rax==rax) return vic[pos].result[i].res;
    require(0,"expected Gate C system call observed");
    return 0;
}
static int64_t first(unsigned pos,uint64_t rax) {
    for(unsigned i=0;i<vic[pos].results;i++) if(vic[pos].result[i].rax==rax) return vic[pos].result[i].res;
    require(0,"expected Gate C system call observed");
    return 0;
}
static int finished(unsigned pos,int state) {
    int s=proc_wait(&procs[pos]);vic[pos].active=0;
    return s==state;
}

void storage_stress_c_progress(struct process *p,const struct exc_frame *f,int64_t result) {
    unsigned pos=PROC_TABLE;
    for(unsigned i=0;i<PROC_TABLE;i++) if(p==&procs[i] && vic[i].active) pos=i;
    if(pos==PROC_TABLE) return;
    struct victim *v=&vic[pos];
    if(f->rax==SYS_FILE_INITIAL && result>0 && !v->initial) v->initial=(uint64_t)result;
    if(f->rax==SYS_FILE_OPEN_AT || f->rax==SYS_FILE_READ_AT || f->rax==SYS_FILE_STAT || f->rax==SYS_DIRECTORY_NEXT) {
        require(v->results<8,"bounded Gate C result record");
        v->result[v->results].rax=f->rax;v->result[v->results++].res=result;
    }
    if(f->rax!=SYS_DEBUG_WRITE) return;
    require(!result && !f->rsi,"existing zero-length debug barrier");
    if(++v->phase==1) {
        struct {uint64_t buffer,op,attempts,region,stale;} cmd;
        require(!copy_from_user(p->root,&cmd,f->rdi,sizeof(cmd)),"validated Gate C configuration record");
        v->buffer=cmd.buffer;cmd.op=v->op;cmd.attempts=v->attempts;cmd.region=v->region;cmd.stale=v->stale;
        require(!user_writable(p->root,v->buffer,4096) && !copy_to_user(p->root,f->rdi,&cmd,sizeof(cmd)),
                "trusted scalar configuration only");
    }
    while(v->phase>=2 && v->release<v->phase && !p->kill_reason) {
        int e=thread_block_until(&barrier_q,irq_ticks()+CASE_TICKS);
        require(e==THREAD_WOKEN || (e==THREAD_INTERRUPTED && p->kill_reason),"Gate C barrier release or authorized kill");
    }
}
/* Test-only publication hold: after the ext2 operation, before the file layer's final cancellation check. */
void storage_stress_c_hold(int op_result) {
    if(hold_state!=1 || thread_current()->id!=hold_tid) return;
    hold_state=2;hold_op_result=op_result;
    hold_wait=io_scope_wait(&hold_q,irq_ticks()+CASE_TICKS);
    hold_killed=hold_proc && hold_proc->kill_reason;     /* the kill, not a deadline, ended the hold */
    hold_state=3;
}

/* ---- user-visible results ---- */
static uint8_t pattern(unsigned role,unsigned region,unsigned j) {
    return (uint8_t)(((0x4e360001U>>(8*(j&3)))+73*role+29*region+17*j+(j>>8))&255);
}
static void user_bytes(unsigned pos,unsigned len) {
    require(procs[pos].state==PROC_RUNNING || procs[pos].state==PROC_EXITED || procs[pos].state==PROC_KILLED,
            "user buffer owner exists");
    require(!copy_from_user(procs[pos].root,scratch,vic[pos].buffer,len),"revalidate observed user range");
}
static void sentinel(unsigned pos,unsigned len) {
    user_bytes(pos,len);
    for(unsigned j=0;j<len;j++) require(scratch[j]==0xa5,"no publication: every destination byte unchanged");
}
static void exact(unsigned pos,uint64_t op,uint64_t region) {
    if(op==OP_READ || op==OP_READ_R1) {
        user_bytes(pos,4096);
        for(unsigned j=0;j<4096;j++) require(scratch[j]==pattern(op==OP_READ_R1,(unsigned)region,j),"exact 4096 data bytes");
    } else if(op==OP_STAT) {
        user_bytes(pos,64);
        uint64_t sizes[2];uint32_t fields[6];memcpy(sizes,scratch,16);memcpy(fields,scratch+16,24);
        require(sizes[0]==69632 && sizes[1]==138 && fields[0]==1 && fields[1]==0100444 && fields[4]==1 &&
                fields[5]==(FILE_READ|FILE_STAT),"exact stat wire record");
        for(unsigned j=40;j<64;j++) require(!scratch[j],"stat reserved bytes zero");
    } else if(op==OP_ENUM) {
        user_bytes(pos,272);
        uint64_t cookie;uint32_t fields[2];memcpy(&cookie,scratch,8);memcpy(fields,scratch+8,8);
        require(cookie==1024 && fields[0]==1 && fields[1]==4 && !memcmp(scratch+16,"only",4),"exact directory entry");
    }
}
static uint64_t syscall_of(uint64_t op) {
    return op==OP_OPEN ? SYS_FILE_OPEN_AT : op==OP_STAT ? SYS_FILE_STAT : op==OP_ENUM ? SYS_DIRECTORY_NEXT : SYS_FILE_READ_AT;
}
static unsigned out_len(uint64_t op) { return op==OP_STAT ? 64 : op==OP_ENUM ? 272 : op==OP_OPEN ? 0 : 4096; }
static int64_t success_of(uint64_t op) { return op==OP_OPEN ? 1 : op==OP_ENUM ? 1 : op==OP_STAT ? 0 : 4096; }
static int succeeded(uint64_t op,int64_t r) { return op==OP_OPEN ? r>0 : r==success_of(op); }

/* ---- device and ticket observation ---- */
static int pending_slot(unsigned pos) {
    capture();
    for(unsigned j=0;j<VBLK_SLOTS;j++)
        if(sample.driver.slots[j].inflight && sample.driver.slots[j].tid==procs[pos].tid) return (int)j;
    return -1;
}
static int held_at_device(unsigned pos,int *slot,uint64_t *ticket) {
    for(unsigned i=0;i<4096;i++) {
        int s=pending_slot(pos);
        if(s>=0) {*slot=s;*ticket=sample.driver.slots[s].ticket;return 1;}
        thread_yield();             /* pre-notify: no tick passes while the request is held */
    }
    return 0;
}
static int queued_at_reader(unsigned pos) {
    for(unsigned i=0;i<4096;i++) {
        capture();
        struct thread *t=thread_test_get(procs[pos].tid);
        for(unsigned r=0;r<EXT2_MAX_FS;r++)
            if(sample.readers[r].busy && t && sample.readers[r].tail==(uint64_t)t && t->state==THREAD_BLOCKED &&
               file_process_busy(&procs[pos])) return 1;
        thread_yield();
    }
    return 0;
}
static unsigned retire_events(uint64_t ticket) {
    unsigned n=0;
    require(!storage_stress_trace.overflow,"complete bounded trace");
    for(unsigned i=0;i<storage_stress_trace.count;i++)
        if(storage_stress_trace.events[i].kind==STRESS_RETIRE && storage_stress_trace.events[i].identity==ticket) n++;
    return n;
}
static void drain_debt(void) {
    uint64_t deadline=pace_deadline();
    for(unsigned i=0;pacing(i,deadline) && (vblk_abandoned() || io_scope_count());i++) pace();
    require(!vblk_abandoned() && !io_scope_count(),"orphan debt retired at the next completion");
}
static void finish_program(unsigned pos,unsigned barrier) {
    wait_barrier(pos,barrier);release(pos);
    require(finished(pos,PROC_EXITED) && !procs[pos].exit_status,"Gate C program verified its own results");
}

/* ---- SC03: kill/completion ordering at two wait sites, two reuse cases ---- */
static void sc03_case(const struct boot_info *bi,unsigned n,uint64_t op,unsigned site,unsigned order,unsigned reuse) {
    static const char *ops[]={"open","read","stat","enum"};
    blk_id_t dev=vblk_info()->blk_id;int irq=vblk_info()->mode==VBLK_MODE_IRQ;
    attach(bi,dev);
    victim(bi,V,op,1,n%3,0);wait_barrier(V,2);
    if(site==SITE_READER) {victim(bi,H,OP_READ_R1,1,(n+1)%3,0);wait_barrier(H,2);}
    require(!blk_invalidate(dev),"cold data path without live scope");
    trace_reset();vblk_test_yield_polls(32);
    if(order==COMPLETE_FIRST) {hold_tid=procs[V].tid;hold_proc=&procs[V];hold_killed=0;hold_state=1;}
    int slot=-1,hslot=-1;uint64_t ticket=0,holder_ticket=0;
    vblk_test_skip_notify(1);
    if(site==SITE_DEVICE) {
        release(V);
        require(held_at_device(V,&slot,&ticket) && ticket,"victim request held in flight at the device");
    } else {
        release(H);
        require(held_at_device(H,&hslot,&holder_ticket) && holder_ticket,"holder owns the shared reader with a held request");
        release(V);
        require(queued_at_reader(V),"victim queued at the actual shared ext2 reader");
    }
    int orphan=0;
    if(order==KILL_FIRST) {
        proc_kill(&procs[V],"Gate C SC03 kill-first");
        if(site==SITE_DEVICE && irq) {
            require(finished(V,PROC_KILLED),"killed victim unwinds without its completion");
            capture();
            require(vblk_abandoned()==1 && sample.driver.slots[slot].abandoned && !sample.driver.slots[slot].owner &&
                    sample.driver.slots[slot].ticket==ticket,"interrupt transport: exactly one orphan with scalar debt");
            orphan=1;vblk_test_notify();
        } else {
            vblk_test_notify();
            require(finished(V,PROC_KILLED),"killed victim unwinds after the held completion");
        }
    } else {
        vblk_test_notify();
        uint64_t deadline=pace_deadline();
        for(unsigned i=0;pacing(i,deadline) && hold_state!=2;i++) pace();
        require(hold_state==2 && !hold_op_result,"operation completed privately and is held before publication");
        proc_kill(&procs[V],"Gate C SC03 complete-first");
        require(finished(V,PROC_KILLED) && hold_state==3 && hold_wait==THREAD_INTERRUPTED && hold_killed,
                "kill lands after completion, before publication");
    }
    require(order==COMPLETE_FIRST || hold_state==0,"hold point inert unless armed");
    hold_state=0;hold_tid=0;hold_proc=0;
    require(procs[V].trap.rax==syscall_of(op) && last(V,syscall_of(op))==-FILE_EINTR,
            "exact killed system call, EINTR inside, no user return");
    if(out_len(op)) sentinel(V,out_len(op));
    drain_debt();
    if(orphan) require(retire_events(ticket)==1,"orphan retired exactly once");
    if(site==SITE_READER) {
        finish_program(H,3);
        require(last(H,SYS_FILE_READ_AT)==4096,"holder completed its own read exactly");
        exact(H,OP_READ_R1,vic[H].region);
        proc_destroy(&procs[H]);
    }
    uint64_t stale=vic[V].initial;
    require(stale,"killed incarnation held a real initial token");
    proc_destroy(&procs[V]);
    /* Reuse: a fresh incarnation in the same record, or another record, repeats the operation exactly. */
    unsigned r=reuse==REUSE_SAME ? V : O;
    victim(bi,r,op,1,n%3,stale);wait_barrier(r,2);
    require(first(r,SYS_FILE_STAT)==-VOS_EBADHANDLE,"killed incarnation's token confers no authority in the new one");
    require(!blk_invalidate(dev),"cold reuse path");
    vblk_test_skip_notify(1);release(r);
    int rslot=-1;uint64_t rticket=0;
    require(held_at_device(r,&rslot,&rticket) && rticket>(ticket>holder_ticket?ticket:holder_ticket) &&
            rslot==(site==SITE_DEVICE?slot:hslot),"fresh ticket on the reused lowest driver slot");
    vblk_test_notify();
    finish_program(r,3);
    require(succeeded(op,last(r,syscall_of(op))),"reuse repeats the operation exactly");
    exact(r,op,n%3);
    proc_destroy(&procs[r]);
    vblk_test_yield_polls(VBLK_YIELD_POLLS);
    require(!file_volume_detach(),"per-case volume detach");quiet();
    serial_puts("storage-stress: SC03 CASE ");serial_dec(n);serial_puts(" op=");serial_puts(ops[op]);
    serial_puts(site==SITE_DEVICE?" site=device":" site=reader");serial_puts(order==KILL_FIRST?" order=kill-first":" order=complete-first");
    serial_puts(reuse==REUSE_SAME?" reuse=same":" reuse=other");
    serial_puts(orphan?" orphan=1":" orphan=0");serial_puts(" kill=PASS publication=NONE accounting=PASS\r\n");
}
static void sc03(const struct boot_info *bi) {
    serial_puts("storage-stress: SC03 BEGIN seed=0x4E360003 cases=32\r\n");
    unsigned n=0;
    for(uint64_t op=OP_OPEN;op<=OP_ENUM;op++) for(unsigned s=0;s<2;s++) for(unsigned k=0;k<2;k++) for(unsigned u=0;u<2;u++)
        sc03_case(bi,n++,op,s,k,u);
    require(n==32,"exact 32-case SC03 matrix");
    serial_puts("storage-stress: SC03 PASS cases=32 kills=32 publication=NONE accounting=PASS\r\n");
}

/* ---- SC04: two readers, active (A) or orphaned (O), two orders, two rounds ---- */
static unsigned retire_index(uint64_t ticket) {
    for(unsigned i=0;i<storage_stress_trace.count;i++)
        if(storage_stress_trace.events[i].kind==STRESS_RETIRE && storage_stress_trace.events[i].identity==ticket) return i;
    return ~0u;
}
static void sc04_line(unsigned n,unsigned mix,unsigned order,unsigned round) {
    static const char *mixes[]={"AA","AO","OO"};
    serial_puts("storage-stress: SC04 CASE ");serial_dec(n);serial_puts(" mix=");serial_puts(mixes[mix]);
    serial_puts(" order=");serial_dec(order);serial_puts(" round=");serial_dec(round);
}
static void sc04_case(const struct boot_info *bi,unsigned n,unsigned mix,unsigned order,unsigned round,unsigned *applicable,unsigned *na) {
    if(mix && vblk_info()->mode!=VBLK_MODE_IRQ) {
        sc04_line(n,mix,order,round);
        serial_puts(" NOT APPLICABLE (polling transport: a killed polling waiter leaves no orphan)\r\n");
        (*na)++;return;
    }
    blk_id_t dev=vblk_info()->blk_id;
    attach(bi,dev);
    unsigned first_pos=order,second_pos=order^1;
    victim(bi,first_pos,first_pos?OP_READ_R1:OP_READ,1,(n+round)%17,0);
    victim(bi,second_pos,second_pos?OP_READ_R1:OP_READ,1,(n+round+5)%17,0);
    wait_barrier(first_pos,2);wait_barrier(second_pos,2);
    require(!blk_invalidate(dev),"cold data path without live scope");
    trace_reset();vblk_test_yield_polls(32);
    const unsigned fate[2]={mix==2,mix>=1};            /* AA: A,A  AO: A,O  OO: O,O */
    uint64_t orphan_ticket[2]={0,0};unsigned orphans=0;
    for(unsigned k=0;k<2;k++) {
        unsigned pos=k?second_pos:first_pos;
        if(!fate[k]) {
            release(pos);wait_barrier(pos,3);
            require(last(pos,SYS_FILE_READ_AT)==4096,"active reader completes exactly");
            exact(pos,pos?OP_READ_R1:OP_READ,vic[pos].region);
            continue;
        }
        int slot;uint64_t ticket;
        vblk_test_skip_notify(1);release(pos);
        require(held_at_device(pos,&slot,&ticket) && ticket,"orphan candidate held in flight");
        proc_kill(&procs[pos],"Gate C SC04 orphan");
        require(finished(pos,PROC_KILLED) && last(pos,SYS_FILE_READ_AT)==-FILE_EINTR,"killed reader leaves its request");
        orphan_ticket[orphans++]=ticket;
        require(vblk_abandoned()==orphans && io_scope_count()==orphans && io_scope_count()<=IO_SCOPE_TICKETS,
                "exact orphan debt within two tickets");
    }
    vblk_test_notify();drain_debt();
    for(unsigned k=0;k<orphans;k++) require(retire_events(orphan_ticket[k])==1,"each orphan retired exactly once");
    if(orphans==2) require(orphan_ticket[0]<orphan_ticket[1] && retire_index(orphan_ticket[0])<retire_index(orphan_ticket[1]),
                           "orphans retire in submission order");
    for(unsigned k=0;k<2;k++) {
        unsigned pos=k?second_pos:first_pos;
        if(!fate[k]) {release(pos);require(finished(pos,PROC_EXITED) && !procs[pos].exit_status,"active reader exits cleanly");}
        proc_destroy(&procs[pos]);
    }
    vblk_test_yield_polls(VBLK_YIELD_POLLS);
    require(!file_volume_detach(),"per-case volume detach");quiet();
    (*applicable)++;
    sc04_line(n,mix,order,round);
    serial_puts(" orphans=");serial_dec(orphans);serial_puts(" retire=EXACT accounting=PASS\r\n");
}
static void sc04(const struct boot_info *bi) {
    serial_puts("storage-stress: SC04 BEGIN seed=0x4E360004 cases=12\r\n");
    unsigned n=0,applicable=0,na=0;
    for(unsigned mix=0;mix<3;mix++) for(unsigned order=0;order<2;order++) for(unsigned round=0;round<2;round++)
        sc04_case(bi,n++,mix,order,round,&applicable,&na);
    int irq=vblk_info()->mode==VBLK_MODE_IRQ;
    require(n==12 && applicable+na==12 && applicable==(irq?12u:4u),"exact SC04 matrix by transport");
    serial_puts("storage-stress: SC04 PASS applicable=");serial_dec(applicable);serial_puts(" not_applicable=");
    serial_dec(na);serial_puts(" accounting=PASS\r\n");
}

/* ---- SC05: mock backend faults (the accepted N3.5 approach), two roles, two rounds ---- */
/* Fault and holder regions whose 4 KiB cache lines hold only that file's data blocks, so setup's metadata reads
 * cannot warm them: region 0 of each file shares a line with its directory block (r0: 446, r1: 516).
 * scripts/test-gatec-sc05-cold-regions.py checks these against the admitted disk image. */
#define SC05_REGION(n)        (1u+(n)/6u)     /* r0/data.bin: region 1 in round 0, region 2 in round 1 */
#define SC05_HOLDER_REGION(r) (3u-(r))        /* r1/data.bin: the other cold region */
static void mock_info(void *ctx,struct blk_info *o) {
    (void)ctx;
    *o=(struct blk_info){.state=BLK_DEV_READY,.sectors=16384,.max_sectors=8,.caps=BLK_CAP_READ,.epoch=1};
}
static int mock_read(void *ctx,uint64_t lba,uint32_t count,void *out,uint64_t cap) {
    (void)ctx;
    if(!count || count>8 || cap<count*512u || lba>=16384 || count>16384-lba) return BLK_EINVAL;
    if(md.hold && thread_current()->id==md.hold_tid) {
        md.hold=0;md.held=1;
        int r=io_scope_wait(&mock_q,irq_ticks()+CASE_TICKS);
        md.held=2;
        if(r==THREAD_INTERRUPTED) return BLK_EINTR;
    }
    if(md.armed && thread_current()->id==md.tid && (md.fault==FAULT_FALLBACK || md.fault==FAULT_PREFIX)) {
        if(count>1) {md.multi_failed++;return BLK_EIO;}
        if(md.fault==FAULT_PREFIX && md.single_ok>=2) {md.single_failed++;return BLK_EIO;}
        md.single_ok++;
    }
    memcpy(out,image+lba*512,count*512u);
    return 0;
}
static const struct blk_ops mock_ops={"gatec-mock",mock_info,mock_read,0};
static void wait_held(void) {
    uint64_t deadline=pace_deadline();
    for(unsigned i=0;pacing(i,deadline) && md.held!=1;i++) pace();
    require(md.held==1,"caller blocked inside its own backend read");
}
static void sc05_case(const struct boot_info *bi,unsigned n,unsigned fault,unsigned role,unsigned round) {
    static const char *faults[]={"line-fallback","prefix-eio","eintr"};
    blk_id_t dev;
    require(!blk_register("gatec-mock",&mock_ops,NULL,&dev),"test-only mock backend registered");
    attach(bi,dev);
    require(!blk_invalidate(dev),"cold device before setup: setup warms metadata, fault regions stay cold");
    unsigned region=SC05_REGION(n);                     /* direct blocks only: no uncached indirect lookup */
    memset(&md,0,sizeof(md));
    victim(bi,V,OP_READ,2,region,0);wait_barrier(V,2);
    if(role==ROLE_WAITER) {victim(bi,H,OP_READ_R1,1,SC05_HOLDER_REGION(region),0);wait_barrier(H,2);}
    if(role==ROLE_WAITER) {
        md.hold_tid=procs[H].tid;md.hold=1;
        release(H);wait_held();
        release(V);
        require(queued_at_reader(V),"waiter queued at the actual shared ext2 reader");
        if(fault==FAULT_EINTR) {
            require(thread_interrupt(procs[V].tid)==1,"nonfatal interruption delivered to the queued waiter");
            wait_barrier(V,3);
        } else {
            md.tid=procs[V].tid;md.fault=(int)fault;md.armed=1;
        }
        thread_wake_all(&mock_q);
        wait_barrier(H,3);
        if(fault!=FAULT_EINTR) wait_barrier(V,3);
    } else {
        md.tid=procs[V].tid;md.fault=(int)fault;md.armed=1;
        if(fault==FAULT_EINTR) {md.hold_tid=procs[V].tid;md.hold=1;}
        release(V);
        if(fault==FAULT_EINTR) {
            wait_held();
            require(thread_interrupt(procs[V].tid)==1,"nonfatal interruption delivered to the owner");
        }
        wait_barrier(V,3);
    }
    md.armed=0;md.hold=0;
    int64_t got=last(V,SYS_FILE_READ_AT);
    if(fault==FAULT_FALLBACK) {
        /* All 4 blocks lie in cold lines: each failed line load falls back to exactly its requested sectors. */
        require(got==4096 && md.multi_failed && md.single_ok==8 && !md.single_failed,
                "whole-line failure recovered by exactly 8 per-sector reads");
        exact(V,OP_READ,region);
    } else {
        require(got==(fault==FAULT_PREFIX?-FILE_EIO:-FILE_EINTR),"exact failure result");
        if(fault==FAULT_PREFIX) require(md.single_ok==2 && md.single_failed==1,"failure after a successful private prefix");
        sentinel(V,4096);
    }
    require(!procs[V].kill_reason && procs[V].state==PROC_RUNNING,"failure is not fatal to the caller");
    release(V);wait_barrier(V,4);
    require(last(V,SYS_FILE_READ_AT)==4096,"same-handle retry succeeds");exact(V,OP_READ,region);
    finish_program(V,4);
    if(role==ROLE_WAITER) {
        require(last(H,SYS_FILE_READ_AT)==4096,"holder unaffected by the waiter's fault");exact(H,OP_READ_R1,vic[H].region);
        finish_program(H,3);
        proc_destroy(&procs[H]);
    }
    proc_destroy(&procs[V]);
    require(!file_volume_detach() && !blk_unregister(dev),"per-case detach and mock removal");quiet();
    serial_puts("storage-stress: SC05 CASE ");serial_dec(n);serial_puts(" fault=");serial_puts(faults[fault]);
    serial_puts(role==ROLE_OWNER?" role=owner":" role=waiter");serial_puts(" round=");serial_dec(round);
    serial_puts(fault==FAULT_FALLBACK?" dest=EXACT":" dest=UNCHANGED");serial_puts(" retry=PASS accounting=PASS\r\n");
}
static void sc05(const struct boot_info *bi) {
    serial_puts("storage-stress: SC05 BEGIN seed=0x4E360005 cases=12\r\n");
    unsigned n=0;
    for(unsigned round=0;round<2;round++) for(unsigned fault=0;fault<3;fault++) for(unsigned role=0;role<2;role++)
        sc05_case(bi,n++,fault,role,round);
    require(n==12,"exact 12-case SC05 matrix");
    serial_puts("storage-stress: SC05 PASS cases=12 accounting=PASS\r\n");
}

int selftest_storage_stress_c(const struct boot_info *bi) {
    /* Fixed-order selector over the Gate C groups only; later groups are never labelled PASS. */
    const char *a=bi->cmdline?bi->cmdline:"";
    int run3=same(a,"n36-groups=SC03") || same(a,"n36-groups=SC03,SC04") || same(a,"n36-groups=SC03,SC05") ||
             same(a,"n36-groups=SC03,SC04,SC05");
    int run4=same(a,"n36-groups=SC04") || same(a,"n36-groups=SC03,SC04") || same(a,"n36-groups=SC04,SC05") ||
             same(a,"n36-groups=SC03,SC04,SC05");
    int run5=same(a,"n36-groups=SC05") || same(a,"n36-groups=SC03,SC05") || same(a,"n36-groups=SC04,SC05") ||
             same(a,"n36-groups=SC03,SC04,SC05");
    require(run3||run4||run5,"explicit supported fixed-order Gate C group selection; SC01/02/10/11 not in this image");
    const struct boot_module *m=module(bi,"storage-stress.img");
    require(m && m->end-m->start==8388608 && vblk_info()->capacity==16384 &&
            vblk_info()->state==VBLK_READY,"accepted immutable fixture and real ready transport");
    image=phys_to_virt(m->start);
    require(!proc_count() && !file_session_count() && !ext2_open_count() && !channel_count(),
            "existing prerequisite tests leave declared baseline");
    snapshot_baseline();quiet();
    serial_puts("storage-stress: TRANSPORT ");serial_puts(vblk_info()->mode==VBLK_MODE_IRQ?"IRQ":"POLL");
    serial_puts(" PASS\r\n");
    if(run3) sc03(bi);
    if(run4) sc04(bi);
    if(run5) sc05(bi);
    quiet();
    /* Every group this boot did not execute, in fixed order. */
    serial_puts("storage-stress: SC01 SC02");
    if(!run3) serial_puts(" SC03");
    if(!run4) serial_puts(" SC04");
    if(!run5) serial_puts(" SC05");
    serial_puts(" SC10 SC11 NOT RUN\r\n");
    serial_puts("storage-stress: CHECKS ");serial_dec(checks);serial_puts(" PASS\r\n");
    serial_puts("storage-stress: SELECTED PASS\r\n");
    return 0;
}
