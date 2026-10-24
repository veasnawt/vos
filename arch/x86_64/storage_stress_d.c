/* Gate D only: SC10 (work-budget exhaustion) and SC11 (directory enumeration) over existing observations.
 * Expected costs come from an independent model of the admitted fixture (raw image bytes, not the ext2 reader)
 * under the source's charging rules: one logical unit per ext2 device access, cache hits included. Each
 * operation's consumption is measured exactly from its scope end (storage_stress_d_event).
 */
#include "storage_stress.h"
#include "storage_observe.h"
#include "blockdev.h"
#include "console.h"
#include "exception.h"
#include "file.h"
#include "io_scope.h"
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

enum { OP_OPEN, OP_ENUM, OP_PROBE, OP_CHECK };
enum { CHECK_CLOSED, CHECK_FILE, CHECK_SIZE };
#define V 0                 /* first program position */
#define H 1                 /* second, independent program position */
/* Each wait for one case's progress may use at most a 1/32 share of the 240-second group watchdog. */
#define CASE_TICKS (240u*PIT_HZ/32u)
#define PACE_ROUNDS 4096u
#define PACE_TICKS  (4u*VBLK_WAIT_TICKS)
#define RECORD 272u
#define MANY 200u           /* /many entries in the accepted fixture */
#define ENDS 64u

struct command { uint64_t buffer, path, op, attempts, len, cookie, mode, reserved; };

static struct process procs[PROC_TABLE];
static struct storage_observation sample;
static uint64_t allocation[PMM_WORDS(PMM_MAX_FRAMES)];
static struct {
    struct observe_thread threads[THREAD_MAX];
    uint64_t cache[BLK_CACHE_LINES], pool[VBLK_POOL_FRAMES], free, usable;
    unsigned devices;
} baseline;
static struct victim {
    unsigned active, phase, release, enumerating, calls, entries, eofs, scoped, setups;
    uint64_t config, buffer, path, seen, rax, consumed, reads, cost, read_total, setup[5];
    int64_t res;
    int end_result;
    struct command next;
    char text[EXT2_PATH_MAX];
} vic[PROC_TABLE];
/* Every scope end (the file call's exact logical units and device reads), kept apart from the bounded trace. */
static struct { uint64_t seq, owner, logical, reads; int result; } ends[ENDS];
static uint64_t end_seq;
static struct wait_queue barrier_q;
static unsigned checks;
static uint64_t prior_pid[PROC_TABLE];
static const uint8_t exit_code[]={0x31,0xc0,0x31,0xff,0xcd,0x80,0x0f,0x0b};
static const uint8_t *image;
static uint32_t fx_bs, fx_ipg, fx_isz, fx_ptrs, scope_ino, many_ino, huge_ino;
static uint64_t fx_gdt;
/* The model of one SYS_DIRECTORY_NEXT call: result (1 entry, 0 EOF, -VOS_EINVAL), logical units, next cookie. */
struct nx { int64_t result; uint64_t cost, next, rec; };
static struct nx many[MANY + 1];
static uint64_t many_cost;
static uint8_t scratch[4096];
_Static_assert(sizeof(procs)+sizeof(sample)+sizeof(allocation)+sizeof(baseline)+sizeof(vic)+sizeof(scratch)+sizeof(ends)+
               sizeof(many)+sizeof(struct stress_trace)+1024 <= 112*1024,"Gate D complete bounded static subset");

static int same(const char *a,const char *b) { return strlen(a)==strlen(b) && !memcmp(a,b,strlen(a)); }
static void require(int ok,const char *why) {
    checks++;
    if(!ok) {
        serial_puts("storage-stress: FAIL: ");serial_puts(why);serial_puts("\r\n");
        panic("Gate D failed; architectural review required");
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

/* ---- the independent fixture model (raw image bytes) ---- */
static uint32_t le32(uint64_t off) { uint32_t v;memcpy(&v,image+off,4);return v; }
static uint16_t le16(uint64_t off) { uint16_t v;memcpy(&v,image+off,2);return v; }
static void fx_init(void) {
    fx_bs=1024u<<le32(1024+24);fx_ipg=le32(1024+40);fx_isz=le16(1024+88);fx_ptrs=fx_bs/4;
    fx_gdt=(uint64_t)(le32(1024+20)+1)*fx_bs;
    require(fx_bs==1024 && fx_isz==256 && le16(1024+56)==0xEF53,"fixture model: accepted 1 KiB ext2 geometry");
}
static uint64_t fx_inode(uint32_t ino) {
    uint32_t g=(ino-1)/fx_ipg,i=(ino-1)%fx_ipg;
    return (uint64_t)le32(fx_gdt+32*g+8)*fx_bs+(uint64_t)i*fx_isz;
}
static uint64_t fx_size(uint32_t ino) { return le32(fx_inode(ino)+4); }
static uint64_t fx_block(uint32_t ino,uint64_t lb) {
    uint64_t in=fx_inode(ino)+40;
    if(lb<12) return (uint64_t)le32(in+4*lb)*fx_bs;
    lb-=12;
    if(lb<fx_ptrs) return (uint64_t)le32((uint64_t)le32(in+48)*fx_bs+4*lb)*fx_bs;
    lb-=fx_ptrs;
    require(lb<(uint64_t)fx_ptrs*fx_ptrs,"fixture model: double-indirect range");
    uint64_t l1=(uint64_t)le32((uint64_t)le32(in+52)*fx_bs+4*(lb/fx_ptrs))*fx_bs;
    return (uint64_t)le32(l1+4*(lb%fx_ptrs))*fx_bs;
}
static unsigned bmap_cost(uint64_t lb) { return lb<12 ? 0 : lb<12+(uint64_t)fx_ptrs ? 1 : 2; }
static int dot(uint64_t rec) {
    uint8_t nl=image[rec+6];
    return (nl==1 && image[rec+8]=='.') || (nl==2 && image[rec+8]=='.' && image[rec+9]=='.');
}
/* ext2_lookup: 2 (directory inode) + 2 (step's directory inode) + scanned blocks + 2 (child inode). */
static uint32_t fx_lookup(uint32_t dir,const char *name,unsigned len,uint64_t *cost) {
    uint64_t c=4;
    for(uint64_t lb=0;lb<fx_size(dir)/fx_bs;lb++) {
        uint64_t b=fx_block(dir,lb);
        c+=bmap_cost(lb)+1;
        for(uint32_t p=0;p<fx_bs;p+=le16(b+p+4)) {
            require(le16(b+p+4)>=8,"fixture model: record length");
            if(le32(b+p) && image[b+p+6]==len && !memcmp(image+b+p+8,name,len)) {*cost=c+2;return le32(b+p);}
        }
    }
    require(0,"fixture model: name present");
    return 0;
}
/* open_at's walk from a granted base: node_stat(base) 2 + per component (lookup + node_stat(child) 2). */
static uint64_t fx_walk(uint32_t base,const char *path,unsigned len) {
    uint64_t cost=2,c;uint32_t ino=base;
    for(unsigned i=0;i<len;) {
        unsigned s=i;
        while(i<len && path[i]!='/') i++;
        ino=fx_lookup(ino,path+s,i-s,&c);cost+=c+2;
        if(i<len) i++;
    }
    (void)ino;
    return cost;
}
/* file.c next_entry + ext2_readdir: node_stat(dir) 2; per readdir call 2 + blocks scanned; node_stat(entry) 2. */
static struct nx fx_next(uint32_t dir,uint64_t cookie) {
    struct nx r={0,2,0,0};
    uint64_t size=fx_size(dir),pos=cookie;
    if(cookie>size) {r.result=-VOS_EINVAL;return r;}
    while(pos<size) {
        uint64_t lb=pos/fx_bs,b=fx_block(dir,lb);
        uint32_t want=(uint32_t)(pos%fx_bs),p=0;
        r.cost+=2+bmap_cost(lb)+1;
        while(p<want) p+=le16(b+p+4);
        if(p!=want) {r.result=-VOS_EINVAL;return r;}
        for(;;) {
            while(p<fx_bs && !le32(b+p)) p+=le16(b+p+4);
            if(p<fx_bs) break;
            if(++lb*fx_bs>=size) {r.next=size;return r;}
            b=fx_block(dir,lb);p=0;r.cost+=bmap_cost(lb)+1;
        }
        pos=lb*fx_bs+p+le16(b+p+4);
        if(dot(b+p)) continue;
        r.cost+=2;r.result=1;r.next=pos;r.rec=b+p;
        return r;
    }
    r.next=pos;
    return r;
}

/* ---- programs ---- */
static void attach(blk_id_t device) {
    require(!proc_create(&procs[V],"gd-setup",exit_code,sizeof(exit_code)),"temporary registered setup");
    require(!file_volume_attach(&procs[V],device),"real read-only transport attach");
    proc_destroy(&procs[V]);
}
static void grants(unsigned pos) {
    const struct file_grant g[2]={{0,FILE_RIGHTS,"scope",5},{1,FILE_RIGHTS,"many",4}};
    require(!file_grants_install(&procs[pos],g,2),"explicit scope and /many grants");
}
/* Load the Gate D program at pos with `attempts` attempts, each behind its own barrier. */
static void program(const struct boot_info *bi,unsigned pos,uint64_t attempts) {
    const struct boot_module *m=module(bi,"stress-gated");struct elf_image img;int error;
    require(m && !load_elf(&procs[pos],"stress-gated",phys_to_virt(m->start),m->end-m->start,&img,&error) &&
            procs[pos].abi==2,"independent ABI v2 Gate D program");
    require(procs[pos].pid>prior_pid[pos],"nonreused PID in reused process record");
    prior_pid[pos]=procs[pos].pid;
    memset(&vic[pos],0,sizeof(vic[pos]));
    vic[pos].active=1;vic[pos].next.attempts=attempts;vic[pos].seen=end_seq;
    grants(pos);
    vic[pos].seen=end_seq;                     /* the grants' own scopes are not the program's */
    require(!proc_start(&procs[pos]),"start Gate D program");
}
/* The parameters of the next attempt, written to the program when it leaves its current barrier. */
static void set_open(unsigned pos,unsigned depth) {
    static const char base[]="deep";
    unsigned n=0;
    memcpy(vic[pos].text,base,4);n=4;
    for(unsigned d=1;d<depth;d++) {
        vic[pos].text[n++]='/';vic[pos].text[n++]='d';
        vic[pos].text[n++]=(char)('0'+d/10);vic[pos].text[n++]=(char)('0'+d%10);
    }
    vic[pos].next.op=OP_OPEN;vic[pos].next.len=n;
}
static void set_op(unsigned pos,uint64_t op,uint64_t cookie,uint64_t mode) {
    vic[pos].next.op=op;vic[pos].next.cookie=cookie;vic[pos].next.mode=mode;vic[pos].next.len=0;
}
static void wait_barrier(unsigned pos,unsigned k) {
    uint64_t deadline=pace_deadline();
    for(unsigned i=0;pacing(i,deadline) &&
        !(vic[pos].phase==k && thread_state(procs[pos].tid)==THREAD_BLOCKED);i++) pace();
    require(vic[pos].phase==k && thread_state(procs[pos].tid)==THREAD_BLOCKED,"exact Gate D barrier reached");
}
static void release(unsigned pos) { vic[pos].release=vic[pos].phase;thread_wake_all(&barrier_q); }
static int finished(unsigned pos,int state) {
    int s=proc_wait(&procs[pos]);vic[pos].active=0;
    return s==state;
}
static void finish_program(unsigned pos,unsigned barrier) {
    wait_barrier(pos,barrier);release(pos);
    require(finished(pos,PROC_EXITED) && !procs[pos].exit_status,"Gate D program verified its own setup");
}
static void user_bytes(unsigned pos,unsigned len) {
    require(procs[pos].state==PROC_RUNNING || procs[pos].state==PROC_EXITED || procs[pos].state==PROC_KILLED,
            "user buffer owner exists");
    require(!copy_from_user(procs[pos].root,scratch,vic[pos].buffer,len),"revalidate observed user range");
}
static void sentinel(unsigned pos) {
    user_bytes(pos,4096);
    for(unsigned j=0;j<4096;j++) require(scratch[j]==0xa5,"no publication: every destination byte unchanged");
}
/* One exact 272-byte record from the fixture, and nothing beyond it. */
static void record_exact(unsigned pos,const struct nx *m) {
    uint64_t c;uint32_t kind,len;uint8_t nl=image[m->rec+6];
    user_bytes(pos,4096);
    memcpy(&c,scratch,8);memcpy(&kind,scratch+8,4);memcpy(&len,scratch+12,4);
    require(c==m->next && kind==image[m->rec+7] && len==nl && !memcmp(scratch+16,image+m->rec+8,nl),
            "exact directory record: cookie, kind and name from the fixture");
    for(unsigned j=16+nl;j<RECORD;j++) require(!scratch[j],"NUL and zero name tail");
    for(unsigned j=RECORD;j<4096;j++) require(scratch[j]==0xa5,"nothing published beyond the 272-byte record");
}
static unsigned published(unsigned pos) {
    unsigned n=0;
    capture();
    for(unsigned i=0;i<PROC_TABLE;i++)
        if(sample.file.sessions[i].pid==procs[pos].pid)
            for(unsigned j=0;j<FILE_MAX;j++) {
                require(sample.file.sessions[i].nodes[j].used!=1,"no node left reserved");
                n+=sample.file.sessions[i].nodes[j].used==2;
            }
    return n;
}

/* Scope ends, recorded for every storage event (even when the bounded trace is full). */
void storage_stress_d_event(unsigned kind,uint64_t identity) {
    if(kind!=STRESS_SCOPE_END) return;
    struct io_scope *s=io_scope_current();
    unsigned i=(unsigned)(end_seq%ENDS);
    ends[i].seq=++end_seq;ends[i].owner=identity;
    ends[i].logical=s ? s->logical_left : 0;ends[i].reads=s ? s->reads_left : 0;ends[i].result=s ? s->result : -1;
}
/* The file call that just returned: its result and, if it opened a scope, exactly what that scope consumed. */
static void observe_call(unsigned pos,struct process *p,const struct exc_frame *f,int64_t result) {
    struct victim *v=&vic[pos];
    require(end_seq-v->seen<=ENDS,"bounded scope-end record");
    v->rax=f->rax;v->res=result;v->scoped=0;v->consumed=v->reads=0;v->end_result=0;
    for(uint64_t q=v->seen;q<end_seq;q++) {
        unsigned i=(unsigned)(q%ENDS);
        if(ends[i].owner!=p->pid) continue;
        v->scoped++;v->consumed=IO_SCOPE_LOGICAL_LIMIT-ends[i].logical;
        v->reads=IO_SCOPE_READ_LIMIT-ends[i].reads;v->end_result=ends[i].result;
    }
    v->seen=end_seq;
    require(v->scoped<=1,"at most one scope per file call");
}
/* An enumeration call, checked as it returns: cookie, result, record or untouched destination, exact cost. */
static void enumeration_call(unsigned pos,const struct exc_frame *f) {
    struct victim *v=&vic[pos];
    unsigned k=v->calls++;
    require(k<=MANY,"at most 200 entries and one EOF per pass");
    const struct nx *m=&many[k];
    require(f->rsi==(k ? many[k-1].next : 0) && f->r10==RECORD,"cookie advances exactly through the returned records");
    require(v->scoped==1 && v->end_result==IO_SCOPE_OK && v->consumed==m->cost,"exact per-call logical units");
    if(k<MANY) {
        require(v->res==1 && m->result==1,"one entry per call");
        record_exact(pos,m);v->entries++;
    } else {
        require(v->res==0 && m->result==0,"EOF after the last entry");
        sentinel(pos);v->eofs++;
    }
    v->cost+=v->consumed;v->read_total+=v->reads;
}
void storage_stress_d_progress(struct process *p,const struct exc_frame *f,int64_t result) {
    unsigned pos=PROC_TABLE;
    for(unsigned i=0;i<PROC_TABLE;i++) if(p==&procs[i] && vic[i].active) pos=i;
    if(pos==PROC_TABLE) return;
    struct victim *v=&vic[pos];
    if(f->rax==SYS_FILE_OPEN_AT || f->rax==SYS_FILE_STAT || f->rax==SYS_DIRECTORY_NEXT || f->rax==SYS_FILE_READ_AT ||
       f->rax==SYS_FILE_INITIAL || f->rax==SYS_HANDLE_CLOSE) {
        observe_call(pos,p,f,result);
        if(v->phase==1 && (f->rax==SYS_FILE_OPEN_AT || f->rax==SYS_HANDLE_CLOSE) && v->setups<5)
            v->setup[v->setups++]=(uint64_t)result;
        if(f->rax==SYS_DIRECTORY_NEXT && v->enumerating) enumeration_call(pos,f);
    }
    if(f->rax!=SYS_DEBUG_WRITE) return;
    require(!result && !f->rsi,"existing zero-length debug barrier");
    if(++v->phase==1) {
        struct command c;
        require(!copy_from_user(p->root,&c,f->rdi,sizeof(c)),"validated Gate D configuration record");
        v->config=f->rdi;v->buffer=c.buffer;v->path=c.path;
        require(!user_writable(p->root,v->buffer,4096) && !user_writable(p->root,v->path,EXT2_PATH_MAX),
                "trusted scalar configuration only");
    }
    while(v->phase>=2 && v->release<v->phase && !p->kill_reason) {
        int e=thread_block_until(&barrier_q,irq_ticks()+CASE_TICKS);
        require(e==THREAD_WOKEN || (e==THREAD_INTERRUPTED && p->kill_reason),"Gate D barrier release or authorized kill");
    }
    if(p->kill_reason) return;
    struct command c=v->next;
    c.buffer=v->buffer;c.path=v->path;
    require(!copy_to_user(p->root,v->config,&c,sizeof(c)) &&
            (c.op!=OP_OPEN || !copy_to_user(p->root,v->path,v->text,c.len)),"next attempt's trusted parameters");
    v->seen=end_seq;
}

/* ---- device holds (as Gate C) ---- */
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
/* Kill the program while its first cold device read is held; the transport decides whether an orphan remains. */
static int kill_held(unsigned pos,uint64_t syscall,const char *why) {
    blk_id_t dev=vblk_info()->blk_id;int irq=vblk_info()->mode==VBLK_MODE_IRQ;
    int slot=-1;uint64_t ticket=0;
    require(!blk_invalidate(dev),"cold path without live scope");
    trace_reset();vblk_test_yield_polls(32);
    vblk_test_skip_notify(1);release(pos);
    require(held_at_device(pos,&slot,&ticket) && ticket,"request held in flight at the device");
    proc_kill(&procs[pos],why);
    int orphan=0;
    if(irq) {
        require(finished(pos,PROC_KILLED),"killed program unwinds without its completion");
        capture();
        require(vblk_abandoned()==1 && sample.driver.slots[slot].abandoned && !sample.driver.slots[slot].owner &&
                sample.driver.slots[slot].ticket==ticket,"interrupt transport: exactly one orphan with scalar debt");
        orphan=1;vblk_test_notify();
    } else {
        vblk_test_notify();
        require(finished(pos,PROC_KILLED),"killed program unwinds after the held completion");
    }
    require(procs[pos].trap.rax==syscall && vic[pos].rax==syscall && vic[pos].res==-FILE_EINTR,
            "exact killed system call, EINTR inside, no user return");
    drain_debt();
    if(orphan) require(retire_events(ticket)==1,"orphan retired exactly once");
    vblk_test_yield_polls(VBLK_YIELD_POLLS);
    return orphan;
}

/* ---- SC10: logical work budget (512) at, below and beyond the limit ---- */
static void sc10_line(unsigned n,const char *kind) {
    serial_puts("storage-stress: SC10 CASE ");serial_dec(n);serial_puts(" kind=");serial_puts(kind);
}
/* One open attempt and its exact accounting: OPEN at the modelled cost, or EAGAIN after all 512 units. */
static int open_attempt(unsigned pos,unsigned barrier,uint64_t want,int cold) {
    unsigned before=published(pos);
    if(cold) require(!blk_invalidate(vblk_info()->blk_id),"cold cache before the attempt");
    release(pos);wait_barrier(pos,barrier);
    struct victim *v=&vic[pos];
    require(v->rax==SYS_FILE_OPEN_AT && v->scoped==1,"one scoped open per attempt");
    if(want<=IO_SCOPE_LOGICAL_LIMIT)
        require(v->res>0 && v->consumed==want && v->end_result==IO_SCOPE_OK && published(pos)==before+1,
                "open within the budget: exact units, one published handle");
    else
        require(v->res==-VOS_EAGAIN && v->consumed==IO_SCOPE_LOGICAL_LIMIT && v->end_result==IO_SCOPE_BUDGET &&
                published(pos)==before,"exhausted budget: EAGAIN after exactly 512 units, nothing published");
    require(cold ? v->reads>0 : v->reads==0,"cold walk reads the device, warm walk reads nothing");
    require(!io_scope_count() && !vblk_abandoned(),"no ticket or request debt after the attempt");
    return want<=IO_SCOPE_LOGICAL_LIMIT;
}
static void sc10_depth(unsigned n,unsigned depth,const struct boot_info *bi,blk_id_t dev) {
    attach(dev);program(bi,V,2);
    set_open(V,depth);wait_barrier(V,2);
    uint64_t want=fx_walk(scope_ino,vic[V].text,(unsigned)vic[V].next.len);
    require(want==2+9u*depth,"modelled cost 2+9d on the single-block chain");
    for(unsigned w=0;w<2;w++) {
        int ok=open_attempt(V,3+w,want,!w);
        sc10_line(n+w,"depth");serial_puts(" depth=");serial_dec(depth);serial_puts(w?" cache=warm":" cache=cold");
        serial_puts(" cost=");serial_dec(want);serial_puts(" consumed=");serial_dec(vic[V].consumed);
        serial_puts(ok?" result=OPEN":" result=EAGAIN");serial_puts(w?" reads=NONE":" reads=DEVICE");
        serial_puts(" accounting=PASS\r\n");
    }
    finish_program(V,4);proc_destroy(&procs[V]);
    require(!file_volume_detach(),"per-case volume detach");quiet();
}
static void sc10_recovery(unsigned n,const struct boot_info *bi,blk_id_t dev) {
    attach(dev);program(bi,V,6);
    set_open(V,56);
    require(fx_walk(scope_ino,vic[V].text,(unsigned)vic[V].next.len)==506,"modelled recovering cost");
    set_open(V,57);
    require(fx_walk(scope_ino,vic[V].text,(unsigned)vic[V].next.len)==515,"modelled exhausting cost");
    wait_barrier(V,2);
    for(unsigned k=0;k<3;k++) {
        set_open(V,57);require(!open_attempt(V,3+2*k,515,1),"exhaustion");
        uint64_t exhausted=vic[V].consumed;
        set_open(V,56);require(open_attempt(V,4+2*k,506,0),"recovery with the full budget");
        sc10_line(n+k,"recovery");serial_puts(" cycle=");serial_dec(k+1);serial_puts(" exhausted=");serial_dec(exhausted);
        serial_puts(" recovered=");serial_dec(vic[V].consumed);serial_puts(" accounting=PASS\r\n");
    }
    finish_program(V,8);proc_destroy(&procs[V]);
    require(!file_volume_detach(),"per-case volume detach");quiet();
}
static void sc10_kill(unsigned n,unsigned depth,const struct boot_info *bi,blk_id_t dev) {
    attach(dev);program(bi,V,1);
    set_open(V,depth);wait_barrier(V,2);
    int orphan=kill_held(V,SYS_FILE_OPEN_AT,"Gate D SC10 kill");
    proc_destroy(&procs[V]);
    require(!file_volume_detach(),"per-case volume detach");quiet();
    sc10_line(n,"kill");serial_puts(" depth=");serial_dec(depth);serial_puts(orphan?" orphan=1":" orphan=0");
    serial_puts(" kill=PASS publication=NONE accounting=PASS\r\n");
}
/* An exhausting program and an independent one: separate scopes, separate budgets, exact results for both. */
static void sc10_isolation(unsigned n,const struct boot_info *bi,blk_id_t dev) {
    attach(dev);program(bi,V,1);program(bi,H,1);
    set_open(V,64);set_open(H,8);wait_barrier(V,2);wait_barrier(H,2);
    unsigned pv=published(V),ph=published(H);
    require(!blk_invalidate(dev),"cold path without live scope");
    trace_reset();vblk_test_yield_polls(32);
    int slot;uint64_t ticket;
    vblk_test_skip_notify(1);release(V);
    require(held_at_device(V,&slot,&ticket),"exhausting program holds the reader at the device");
    release(H);
    require(queued_at_reader(H),"independent program queued at the shared reader");
    vblk_test_notify();
    wait_barrier(V,3);wait_barrier(H,3);
    require(vic[V].res==-VOS_EAGAIN && vic[V].consumed==IO_SCOPE_LOGICAL_LIMIT && vic[V].end_result==IO_SCOPE_BUDGET &&
            published(V)==pv,"exhausting program: EAGAIN after 512 units, nothing published");
    require(vic[H].res>0 && vic[H].consumed==74 && vic[H].end_result==IO_SCOPE_OK && published(H)==ph+1,
            "independent program: its own full budget, exact 74 units, one handle");
    vblk_test_yield_polls(VBLK_YIELD_POLLS);
    finish_program(V,3);finish_program(H,3);proc_destroy(&procs[V]);proc_destroy(&procs[H]);
    require(!file_volume_detach(),"per-case volume detach");quiet();
    sc10_line(n,"isolation");serial_puts(" exhausted=512 independent=74 accounting=PASS\r\n");
}
static void sc10(const struct boot_info *bi) {
    static const unsigned depths[4]={8,56,57,64};
    blk_id_t dev=vblk_info()->blk_id;
    serial_puts("storage-stress: SC10 BEGIN seed=0x4E36000A cases=14\r\n");
    for(unsigned i=0;i<4;i++) sc10_depth(2*i,depths[i],bi,dev);
    sc10_recovery(8,bi,dev);
    sc10_kill(11,56,bi,dev);
    sc10_kill(12,64,bi,dev);
    sc10_isolation(13,bi,dev);
    serial_puts("storage-stress: SC10 PASS cases=14 accounting=PASS\r\n");
}

/* ---- SC11: directory enumeration, cookies and boundaries ---- */
static const char *results(int64_t r,uint64_t cost) {
    return cost>IO_SCOPE_LOGICAL_LIMIT ? "EAGAIN" : r==1 ? "ENTRY" : r==0 ? "EOF" : "EINVAL";
}
static void sc11_pass(unsigned pos,unsigned pass,const char *mode,int cold) {
    struct victim *v=&vic[pos];
    if(cold) require(!blk_invalidate(vblk_info()->blk_id),"cold cache before the pass");
    v->enumerating=1;v->calls=v->entries=v->eofs=0;v->cost=v->read_total=0;
    release(pos);wait_barrier(pos,pos==V ? 2+pass : 3);
    v->enumerating=0;
    require(v->calls==MANY+1 && v->entries==MANY && v->eofs==1 && v->cost==many_cost,"complete pass: 200 entries and EOF");
    require(cold ? v->read_total>0 : v->read_total==0,"cold pass reads the device, warm passes read nothing");
    require(!io_scope_count(),"no ticket debt after the pass");
    serial_puts("storage-stress: SC11 MANY pass=");serial_dec(pass);serial_puts(" mode=");serial_puts(mode);
    serial_puts(" entries=200 eof=1 calls=201 cost=");serial_dec(v->cost);serial_puts(" accounting=PASS\r\n");
}
static void sc11(const struct boot_info *bi) {
    static const uint64_t probes[10]={0,1024,351232,352256,522240,523264,524288,524289,523265,24};
    static const char *checkk[3]={"closed-handle","file-handle","record-size"};
    static const int64_t checkr[3]={-VOS_EBADHANDLE,-FILE_ENOTDIR,-VOS_EINVAL};
    static const char *checkn[3]={"EBADHANDLE","ENOTDIR","EINVAL"};
    blk_id_t dev=vblk_info()->blk_id;
    unsigned calls=0,entries=0,eofs=0,eagain=0,einval=0;
    serial_puts("storage-stress: SC11 BEGIN seed=0x4E36000B calls=814 checks=4\r\n");
    uint64_t cookie=0;
    many_cost=0;
    for(unsigned k=0;k<=MANY;k++) {
        many[k]=fx_next(many_ino,cookie);
        require(k<MANY ? many[k].result==1 : many[k].result==0,"fixture model: 200 entries then EOF");
        require(many[k].cost<=IO_SCOPE_LOGICAL_LIMIT,"fixture model: every /many call within the budget");
        many_cost+=many[k].cost;cookie=many[k].next;
    }
    attach(dev);
    program(bi,V,16);wait_barrier(V,2);
    require(vic[V].setups==5 && (int64_t)vic[V].setup[0]>0 && (int64_t)vic[V].setup[3]>0 && !vic[V].setup[4],
            "program setup: directory, huge, file and a closed handle");
    set_op(V,OP_ENUM,0,0);sc11_pass(V,1,"cold",1);
    set_op(V,OP_ENUM,0,0);sc11_pass(V,2,"warm",0);
    set_op(V,OP_ENUM,0,1);sc11_pass(V,3,"reopened",0);
    program(bi,H,1);wait_barrier(H,2);
    set_op(H,OP_ENUM,0,0);sc11_pass(H,4,"independent",0);
    finish_program(H,3);proc_destroy(&procs[H]);
    calls+=4*(MANY+1);entries+=4*MANY;eofs+=4;
    for(unsigned i=0;i<10;i++) {
        struct nx m=fx_next(huge_ino,probes[i]);
        set_op(V,OP_PROBE,probes[i],0);
        release(V);wait_barrier(V,6+i);
        struct victim *v=&vic[V];
        require(v->rax==SYS_DIRECTORY_NEXT && v->scoped==1,"one scoped probe");
        if(m.cost>IO_SCOPE_LOGICAL_LIMIT) {
            require(v->res==-VOS_EAGAIN && v->consumed==IO_SCOPE_LOGICAL_LIMIT && v->end_result==IO_SCOPE_BUDGET,
                    "probe beyond the budget: EAGAIN after exactly 512 units, no progress");
            sentinel(V);eagain++;
        } else {
            require(v->res==m.result && v->consumed==m.cost && v->end_result==IO_SCOPE_OK,"probe at its exact modelled cost");
            if(m.result==1) {record_exact(V,&m);entries++;} else {sentinel(V);if(m.result==0) eofs++; else einval++;}
        }
        calls++;
        serial_puts("storage-stress: SC11 PROBE ");serial_dec(i);serial_puts(" cookie=");serial_dec(probes[i]);
        serial_puts(" result=");serial_puts(results(m.result,m.cost));serial_puts(" cost=");serial_dec(m.cost);
        serial_puts(" consumed=");serial_dec(v->consumed);
        if(m.cost<=IO_SCOPE_LOGICAL_LIMIT && m.result==1) {
            serial_puts(" name=");
            for(unsigned j=0;j<image[m.rec+6];j++) serial_putc((char)image[m.rec+8+j]);
            serial_puts(" next=");serial_dec(m.next);
        }
        serial_puts(" accounting=PASS\r\n");
    }
    require(calls==814 && entries==803 && eofs==5 && eagain==3 && einval==3,"exact enumeration call accounting");
    serial_puts("storage-stress: SC11 ENUMERATION calls=814 entries=803 eof=5 eagain=3 einval=3 accounting=PASS\r\n");
    for(unsigned i=0;i<3;i++) {
        set_op(V,OP_CHECK,0,i);
        release(V);wait_barrier(V,16+i);
        require(vic[V].rax==SYS_DIRECTORY_NEXT && vic[V].res==checkr[i] && !vic[V].scoped,
                "rejected before any storage work, with the exact error");
        sentinel(V);
        serial_puts("storage-stress: SC11 CHECK ");serial_dec(i);serial_puts(" kind=");serial_puts(checkk[i]);
        serial_puts(" result=");serial_puts(checkn[i]);serial_puts(" dest=UNCHANGED accounting=PASS\r\n");
    }
    finish_program(V,18);proc_destroy(&procs[V]);
    program(bi,V,1);wait_barrier(V,2);
    set_op(V,OP_PROBE,523264,0);
    int orphan=kill_held(V,SYS_DIRECTORY_NEXT,"Gate D SC11 kill");
    sentinel(V);
    proc_destroy(&procs[V]);
    require(!file_volume_detach(),"per-group volume detach");quiet();
    serial_puts(orphan?"storage-stress: SC11 CHECK 3 kind=kill-held-probe orphan=1":
                       "storage-stress: SC11 CHECK 3 kind=kill-held-probe orphan=0");
    serial_puts(" kill=PASS publication=NONE accounting=PASS\r\n");
    serial_puts("storage-stress: SC11 PASS calls=814 checks=4 accounting=PASS\r\n");
}

int selftest_storage_stress_d(const struct boot_info *bi) {
    /* Fixed-order selector over the Gate D groups only; other groups are never labelled PASS. */
    const char *a=bi->cmdline?bi->cmdline:"";
    int run10=same(a,"n36-groups=SC10") || same(a,"n36-groups=SC10,SC11");
    int run11=same(a,"n36-groups=SC11") || same(a,"n36-groups=SC10,SC11");
    require(run10||run11,"explicit supported fixed-order Gate D group selection; SC01-SC05 not in this image");
    const struct boot_module *m=module(bi,"storage-stress.img");
    require(m && m->end-m->start==8388608 && vblk_info()->capacity==16384 &&
            vblk_info()->state==VBLK_READY,"accepted immutable fixture and real ready transport");
    image=phys_to_virt(m->start);
    fx_init();
    uint64_t c;
    scope_ino=fx_lookup(EXT2_ROOT_INO,"scope",5,&c);many_ino=fx_lookup(EXT2_ROOT_INO,"many",4,&c);
    huge_ino=fx_lookup(scope_ino,"huge",4,&c);
    require(fx_size(many_ino)==4096 && fx_size(huge_ino)==524288,"fixture model: /many and /scope/huge geometry");
    require(!proc_count() && !file_session_count() && !ext2_open_count() && !channel_count(),
            "existing prerequisite tests leave declared baseline");
    snapshot_baseline();quiet();
    serial_puts("storage-stress: TRANSPORT ");serial_puts(vblk_info()->mode==VBLK_MODE_IRQ?"IRQ":"POLL");
    serial_puts(" PASS\r\n");
    if(run10) sc10(bi);
    if(run11) sc11(bi);
    quiet();
    /* Every group this boot did not execute, in fixed order. */
    serial_puts("storage-stress: SC01 SC02 SC03 SC04 SC05");
    if(!run10) serial_puts(" SC10");
    if(!run11) serial_puts(" SC11");
    serial_puts(" NOT RUN\r\n");
    serial_puts("storage-stress: CHECKS ");serial_dec(checks);serial_puts(" PASS\r\n");
    serial_puts("storage-stress: SELECTED PASS\r\n");
    return 0;
}
