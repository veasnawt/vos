/* N3.6 Stage 3 Gate A only. No SC group is implemented or executed here. */
#include "storage_stress.h"
#include "storage_observe.h"
#include "console.h"
#include "lib.h"
#include "loader.h"
#include "memlayout.h"
#include "panic.h"
#include "pmm.h"

static struct storage_observation base, after;
static uint64_t allocation[PMM_WORDS(PMM_MAX_FRAMES)];
static struct process caller;
static unsigned checks;
static const uint8_t exit_code[] = {0x31,0xc0,0x31,0xff,0xcd,0x80,0x0f,0x0b};
_Static_assert(sizeof(base)*2 + sizeof(allocation) + sizeof(caller) +
               sizeof(struct stress_trace) <= 96*1024, "Gate A static subset");

static void require(int condition, const char *why)
{
    checks++;
    if (!condition) {
        serial_puts("storage-canary: FAIL: "); serial_puts(why); serial_puts("\r\n");
        panic("N3.6 Gate A canary failed; stop");
    }
}
static const struct boot_module *module(const struct boot_info *bi, const char *name)
{
    for (unsigned i=0;i<bi->module_count;i++)
        if (strlen(bi->modules[i].cmdline)==strlen(name) &&
            !memcmp(bi->modules[i].cmdline,name,strlen(name))) return &bi->modules[i];
    return NULL;
}
static int same_thread(const struct observe_thread *a,const struct observe_thread *b)
{
    return a->record==b->record && a->tid==b->tid && a->process==b->process &&
           a->lo==b->lo && a->hi==b->hi && a->stack==b->stack && a->stack_owner==b->stack_owner;
}
static void quiet(void)
{
    require(!storage_observe(&after), "terminal observation");
    require(!file_session_count() && !file_handle_count() && !after.file.refs &&
            !after.file.active && !after.file.fs && !after.file.config_owner,
            "no file handle/grant/session/configuration owner");
    for (unsigned i=0;i<PROC_TABLE;i++) {
        require(!after.processes[i].record, "no process record remains");
        require(!after.file.sessions[i].owner && !after.file.sessions[i].active,
                "no dead session owner");
        for (unsigned j=0;j<FILE_MAX;j++)
            require(!after.file.sessions[i].nodes[j].used, "no published or reserved node");
    }
    for (unsigned i=0;i<THREAD_MAX;i++)
        require(same_thread(&base.threads[i],&after.threads[i]), "baseline thread/stack identity");
    for (unsigned i=0;i<EXT2_MAX_FS;i++)
        require(!after.readers[i].used && !after.readers[i].busy &&
                !after.readers[i].head && !after.readers[i].tail, "no reader or wait owner");
    for (unsigned i=0;i<IO_SCOPE_TICKETS;i++)
        require(!after.tickets[i].id, "no ticket or detached debt");
    require(!after.block.head && !after.block.tail &&
            after.block.devices_count==base.block.devices_count, "registry/cache wait baseline");
    for (unsigned i=0;i<BLK_CACHE_LINES;i++)
        require(after.block.lines[i].frame==base.block.lines[i].frame &&
                !after.block.lines[i].pins && after.block.lines[i].state!=BLK_LINE_LOADING,
                "persistent cache identity; no pins/loaders");
    require(after.driver.pool_held && !after.driver.quarantined &&
            after.driver.state==VBLK_READY && after.driver.descriptors_available,
            "healthy persistent driver pool");
    for (unsigned i=0;i<VBLK_POOL_FRAMES;i++)
        require(after.driver.pool[i]==base.driver.pool[i], "persistent DMA frame identity");
    for (unsigned i=0;i<VBLK_SLOTS;i++) {
        const struct observe_slot *s=&after.driver.slots[i];
        require(!s->owner && !s->tid && !s->ticket && !s->inflight && !s->done &&
                !s->abandoned && !s->head && !s->tail, "retired request without dangling owner");
    }
    require(after.pmm_free==base.pmm_free && after.pmm_usable==base.pmm_usable &&
            !memcmp(allocation,pmm_kernel()->alloc,sizeof(allocation)) &&
            pmm_check_in(pmm_kernel())==PMM_OK, "exact allocated-frame set restored");
    require(!channel_count() && !channel_audit() && !proc_audit() && !sched_audit() &&
            !file_audit() && !ext2_audit() && !blk_audit() && !vblk_audit() && !io_scope_audit(),
            "all authoritative lifecycle audits");
}

int selftest_storage_stress(const struct boot_info *bi)
{
    const struct boot_module *fixture=module(bi,"storage-stress.img");
    const struct boot_module *program=module(bi,"storage-canary");
    require(bi->module_count==3 && fixture && fixture->end-fixture->start==8*1024*1024 && program,
            "exact declared modules and 8 MiB fixture");
    require(vblk_info()->state==VBLK_READY && vblk_info()->mode==VBLK_MODE_POLL &&
            vblk_info()->capacity==16384, "real read-only UEFI polling disk geometry");
    require(!storage_observe(&base) && !storage_observe(&after) &&
            !memcmp(&base,&after,sizeof(base)), "observer has no state-changing side effects");
    require(!base.file.fs && !file_session_count() && !ext2_open_count() &&
            !channel_count() && pmm_kernel()->frames==PMM_MAX_FRAMES, "declared canary baseline");
    memcpy(allocation,pmm_kernel()->alloc,sizeof(allocation));
    require(!proc_create(&caller,"scope-setup",exit_code,sizeof(exit_code)), "temporary setup owner");
    require(!file_volume_attach(&caller,vblk_info()->blk_id), "real-driver reader attachment");
    proc_destroy(&caller);
    struct elf_image image;
    int error;
    require(!load_elf(&caller,"storage-canary",phys_to_virt(program->start),
                     program->end-program->start,&image,&error) && caller.abi==2,
            "separately built ABI v2 canary");
    const struct file_grant grant={0,FILE_RIGHTS,"scope",5};
    require(!file_grants_install(&caller,&grant,1) && !handles_audit(&caller), "non-root grant only");
    require(!blk_invalidate(vblk_info()->blk_id), "cold actual backend reads");
    require(!io_scope_count(), "no live scope when resetting event recording");
    memset(&storage_stress_trace,0,sizeof(storage_stress_trace));
    uint64_t submitted=vblk_info()->submitted, completed=vblk_info()->completed;
    require(!proc_start(&caller) && proc_wait(&caller)==PROC_EXITED && caller.exit_status==0,
            "two exact reads, full stat/dir records, EOF sentinel and closes");
    require(caller.syscalls==13 && !caller.sys_enosys && !caller.sys_einval && !caller.sys_efault,
            "exact 13 successful returning ABI calls");
    struct stress_trace *trace=&storage_stress_trace;
    require(!trace->overflow && trace->kinds[STRESS_PROGRESS]==13 &&
            trace->kinds[STRESS_SCOPE_END]==8, "complete bounded syscall/scope trace");
    require(trace->kinds[STRESS_POLL]>0 &&
            trace->kinds[STRESS_POLL]==trace->kinds[STRESS_COMPLETE] &&
            trace->kinds[STRESS_COMPLETE]==trace->kinds[STRESS_RETIRE] &&
            trace->kinds[STRESS_RETIRE]==vblk_info()->submitted-submitted &&
            trace->kinds[STRESS_RETIRE]==vblk_info()->completed-completed,
            "actual polling/completion/retirement one-to-one");
    for (unsigned i=0;i<trace->count;i++) {
        const struct stress_event *e=&trace->events[i];
        require(e->sequence==i+1 && e->tid==caller.tid && e->kind<STRESS_EVENT_KINDS,
                "event sequence and exact caller TID");
        if (e->kind==STRESS_PROGRESS)
            require(e->identity==caller.pid, "progress process identity");
        else
            require(e->ticket && e->logical<=512 && e->reads<=256 && e->waits<=1024 &&
                    !e->result, "live bounded operation ticket with successful outcome");
    }
    serial_puts("storage-canary: ABI calls=13 scopes=8 reads=2 wire=64/272 EOF preserved PASS\r\n");
    serial_puts("storage-canary: genuine polling requests="); serial_dec(trace->kinds[STRESS_POLL]);
    serial_puts(" completions/retirements exact PASS\r\n");
    proc_destroy(&caller);
    require(!file_volume_detach(), "reader detached after process reclamation");
    quiet();
    serial_puts("storage-canary: exact frames/identities/owners restored PASS\r\n");
    serial_puts("storage-canary: checks="); serial_dec(checks); serial_puts(" PASS\r\n");
    serial_puts("storage-canary: SC01 SC02 SC03 SC04 SC05 SC10 SC11 NOT RUN\r\n");
    serial_puts("storage-canary: GATE A PASS\r\n");
    return 0;
}
