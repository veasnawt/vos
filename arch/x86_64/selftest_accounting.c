/* N3.6 I2 only: exactly three smoke scenarios; no stress or new injections. */
#include "storage_observe.h"
#include "console.h"
#include "cpu.h"
#include "exception.h"
#include "irq.h"
#include "lib.h"
#include "loader.h"
#include "memlayout.h"
#include "panic.h"
#include "pmm.h"
#include "syscall.h"

static struct storage_observation base, sample, repeat;
static uint64_t allocation[PMM_WORDS(PMM_MAX_FRAMES)];
static struct process caller, controller;
static struct observe_driver descriptor_before;
static struct observe_thread expected_thread;
static const uint8_t exit_code[] = {0x31,0xc0,0x31,0xff,0xcd,0x80,0x0f,0x0b};
static struct wait_queue gate;
static const uint8_t *fixture;
static uint64_t fixture_bytes, backend_reads, last_pid, last_tid;
static int hold, fail_reads;
static unsigned checks;
static blk_id_t ram_device;
_Static_assert(sizeof(base) * 3 + sizeof(allocation) + sizeof(caller) + sizeof(controller) +
               sizeof(descriptor_before) + sizeof(expected_thread)
               <= 96 * 1024, "accounting test static bound");

static void require(int ok, const char *why)
{
    checks++;
    if (!ok) {
        serial_puts("accounting: FAIL: ");
        serial_puts(why);
        serial_puts("\r\n");
        panic("accounting smoke failed; stop");
    }
}
static void capture(struct storage_observation *out)
{
    require(storage_observe(out) == 0, "IF=0 thread-context snapshot");
}
static void unchanged_observer(void)
{
    capture(&sample);
    capture(&repeat);
    require(!memcmp(&sample, &repeat, sizeof(sample)), "observer has no CPU-state side effects");
}
static unsigned processes(const struct storage_observation *s)
{
    unsigned n = 0;
    for (unsigned i = 0; i < PROC_TABLE; i++) n += s->processes[i].record != 0;
    return n;
}
static unsigned threads(const struct storage_observation *s)
{
    unsigned n = 0;
    for (unsigned i = 0; i < THREAD_MAX; i++) n += s->threads[i].record != 0;
    return n;
}
/* Predicates inspect snapshot scalars only; never dereference recorded addresses. */
static int same_thread_identity(const struct observe_thread *a, const struct observe_thread *b)
{
    return a->record == b->record && a->tid == b->tid && a->process == b->process &&
           a->lo == b->lo && a->hi == b->hi && a->stack == b->stack &&
           a->stack_owner == b->stack_owner;
}
static int baseline_identities(const struct storage_observation *s)
{
    for (unsigned i = 0; i < THREAD_MAX; i++)
        if (!same_thread_identity(&s->threads[i], &base.threads[i]))
            return 0;
    return 1; /* pool slots/identities stable; RSP, state and queue links legitimately move */
}
static int process_identity(const struct observe_process *s, const struct process *p)
{
    return s->record == (uint64_t)p && s->pid == p->pid && s->tid == p->tid &&
           s->root == p->root && s->stack_top == p->kstack_top &&
           s->pages == p->npages && s->state == (unsigned)p->state;
}
static int active_identities(const struct storage_observation *s)
{
    unsigned nc = 0, nk = 0, nt = 0;
    for (unsigned i = 0; i < PROC_TABLE; i++) {
        const struct observe_process *p = &s->processes[i];
        if (!p->record) continue;
        if (process_identity(p, &caller)) nc++;
        else if (process_identity(p, &controller)) nk++;
        else return 0;
    }
    if (nc != 1 || nk != 1 || !caller.pid || caller.pid == controller.pid) return 0;
    for (unsigned i = 0; i < THREAD_MAX; i++) {
        const struct observe_thread *t = &s->threads[i];
        if (!t->record) continue;
        if (!t->tid) return 0;
        for (unsigned j = 0; j < i; j++)
            if (s->threads[j].record == t->record || s->threads[j].tid == t->tid) return 0;
        if (t->tid == caller.tid) {
            nt++;
            if (!same_thread_identity(t, &expected_thread) ||
                t->process != (uint64_t)&caller || t->stack_owner != t->record ||
                t->lo >= t->rsp || t->rsp >= t->hi || t->hi != caller.kstack_top ||
                (t->state != THREAD_BLOCKED && t->state != THREAD_RUNNABLE)) return 0;
        } else if (t->process == (uint64_t)&caller || t->process == (uint64_t)&controller) return 0;
    }
    return nt == 1;
}
static void active_negative_checks(void)
{
    unsigned ti = THREAD_MAX, pi = PROC_TABLE, free = THREAD_MAX;
    require(active_identities(&sample), "positive ACTIVE identity predicate");
    for (unsigned i = 0; i < THREAD_MAX; i++) {
        if (sample.threads[i].tid == caller.tid) ti = i;
        if (!sample.threads[i].record) free = i;
    }
    for (unsigned i = 0; i < PROC_TABLE; i++)
        if (sample.processes[i].record == (uint64_t)&caller) pi = i;
    require(ti < THREAD_MAX && free < THREAD_MAX && pi < PROC_TABLE, "negative-test snapshot slots");
    repeat = sample;
    memset(&repeat.threads[ti], 0, sizeof(repeat.threads[ti]));
    require(!active_identities(&repeat), "reject missing caller thread");
    repeat = sample;
    repeat.processes[pi].pid ^= 1;
    require(!active_identities(&repeat), "reject wrong process PID");
    repeat = sample;
    repeat.processes[pi].record ^= 1;
    require(!active_identities(&repeat), "reject wrong process record without dereferencing it");
    repeat = sample;
    repeat.threads[free] = repeat.threads[ti];
    require(!active_identities(&repeat), "reject duplicate expected thread");
    repeat = sample;
    repeat.threads[ti].stack_owner ^= 1;
    require(!active_identities(&repeat), "reject wrong active stack owner");
    repeat = sample;
    repeat.threads[ti].tid ^= 1ULL << 63;
    require(!active_identities(&repeat), "reject stale/reused thread identity");
}
static void observer_negative_checks(void)
{
    capture(&sample);
    require(baseline_identities(&sample), "positive baseline identity predicate");
    unsigned ti = THREAD_MAX;
    for (unsigned i = 0; i < THREAD_MAX; i++)
        if (sample.threads[i].record && sample.threads[i].stack_owner) ti = i;
    require(ti < THREAD_MAX, "baseline owned stack for altered-copy tests");
    repeat = sample;
    repeat.threads[ti].stack_owner ^= 1;
    require(threads(&repeat) == threads(&base) && !baseline_identities(&repeat),
            "reject same counts with different baseline stack ownership");
    repeat = sample;
    repeat.threads[ti].tid ^= 1ULL << 63;
    require(threads(&repeat) == threads(&base) && !baseline_identities(&repeat),
            "reject same counts with reused baseline identity");
    repeat = sample;
    memset(&repeat.threads[ti], 0, sizeof(repeat.threads[ti]));
    require(!baseline_identities(&repeat), "reject missing baseline thread");

    descriptor_before = sample.driver;
    require(descriptor_before.pool_held && !descriptor_before.quarantined &&
            descriptor_before.descriptors_available, "synthetic missing-table test has allocated real pool");
    vblk_test_observe_hide_table(1);
    capture(&sample);
    capture(&repeat);
    vblk_test_observe_hide_table(0); /* before assertions; no real queue/device state was changed */
    require(!memcmp(&sample, &repeat, sizeof(sample)), "missing-table observations have no CPU-state side effects");
    require(sample.driver.pool_held && !sample.driver.descriptors_available &&
            !memcmp(sample.driver.pool, descriptor_before.pool, sizeof(descriptor_before.pool)),
            "allocated pool remains charged with descriptors explicitly unavailable");
    for (unsigned i = 0; i < VBLK_QUEUE_SIZE; i++)
        require(!sample.driver.descriptors[i].address && !sample.driver.descriptors[i].length &&
                !sample.driver.descriptors[i].flags && !sample.driver.descriptors[i].next,
                "unavailable descriptors are zero, never invented");
    for (unsigned i = 0; i < VBLK_SLOTS; i++)
        require(!sample.driver.slots[i].inflight && !sample.driver.slots[i].done &&
                !sample.driver.slots[i].abandoned &&
                !memcmp(&sample.driver.slots[i], &descriptor_before.slots[i], sizeof(struct observe_slot)),
                "synthetic descriptor absence preserves idle request identities");
    capture(&sample);
    require(!memcmp(&sample.driver, &descriptor_before, sizeof(descriptor_before)),
            "synthetic filter restoration preserves complete real driver observation");
    require(!memcmp(allocation, pmm_kernel()->alloc, sizeof(allocation)),
            "missing-table observation cannot change pool allocation");
    serial_puts("accounting: synthetic missing-table and baseline identity negatives PASS\r\n");
}
static void frames(void)
{
    const struct pmm *p = pmm_kernel();
    require(pmm_check_in(p) == PMM_OK && p->free == base.pmm_free &&
            p->usable == base.pmm_usable &&
            !memcmp(allocation, p->alloc, sizeof(allocation)), "exact allocated-frame set restored");
    for (unsigned i = 0; i < BLK_CACHE_LINES; i++)
        require(sample.block.lines[i].frame == base.block.lines[i].frame &&
                pmm_frame_state_in(p, sample.block.lines[i].frame) == PMM_FRAME_ALLOCATED,
                "persistent cache frame identity and allocation");
    for (unsigned i = 0; i < VBLK_POOL_FRAMES; i++)
        require(sample.driver.pool[i] == base.driver.pool[i] &&
                pmm_frame_state_in(p, sample.driver.pool[i]) == PMM_FRAME_ALLOCATED,
                "persistent DMA frame identity and allocation");
}
static void quiet(unsigned devices, unsigned readers)
{
    capture(&sample);
    require(!processes(&sample) && threads(&sample) == threads(&base) && baseline_identities(&sample),
            "no smoke process/thread remains");
    require(!sample.file.refs && !sample.file.active && !sample.file.config_owner &&
            !file_session_count() && !file_handle_count(), "no file session/node/grant/reservation remains");
    for (unsigned i = 0; i < PROC_TABLE; i++)
        for (unsigned j = 0; j < FILE_MAX; j++)
            require(!sample.file.sessions[i].nodes[j].used, "no unpublished node remains");
    for (unsigned i = 0; i < IO_SCOPE_TICKETS; i++)
        require(!sample.tickets[i].id, "no admission ticket or debt remains");
    unsigned open = 0;
    for (unsigned i = 0; i < EXT2_MAX_FS; i++) {
        const struct observe_reader *r = &sample.readers[i];
        open += r->used != 0;
        require(!r->busy && !r->head && !r->tail, "reader quiescent");
    }
    require(open == readers && sample.block.devices_count == devices &&
            !sample.block.head && !sample.block.tail, "configured reader/device baseline");
    for (unsigned i = 0; i < BLK_CACHE_LINES; i++)
        require(sample.block.lines[i].state != BLK_LINE_LOADING &&
                !sample.block.lines[i].pins, "no loading or pinned cache line");
    require(sample.driver.pool_held && !sample.driver.quarantined, "ordinary baseline is retained DMA, not quarantine");
    for (unsigned i = 0; i < VBLK_SLOTS; i++) {
        const struct observe_slot *s = &sample.driver.slots[i];
        require(!s->inflight && !s->done && !s->abandoned && !s->owner &&
                !s->ticket && !s->head && !s->tail, "no request/owner/waiter survives");
    }
    frames();
    require(!proc_audit() && !sched_audit() && !file_audit() && !ext2_audit() &&
            !blk_audit() && !vblk_audit(), "quiescent authoritative audits");
    unchanged_observer();
}
static int blocked(uint64_t tid)
{
    for (unsigned i = 0; i < 100 && thread_state(tid) != THREAD_BLOCKED; i++)
        thread_yield();
    return thread_state(tid) == THREAD_BLOCKED;
}
static const struct boot_module *module(const struct boot_info *bi, const char *name)
{
    for (unsigned i = 0; i < bi->module_count; i++)
        if (strlen(bi->modules[i].cmdline) == strlen(name) && !memcmp(bi->modules[i].cmdline, name, strlen(name))) return &bi->modules[i];
    return NULL;
}
static void attach(blk_id_t device)
{
    require(!proc_create(&caller, "n36-setup", exit_code, sizeof(exit_code)), "setup owner");
    require(!file_volume_attach(&caller, device), "explicit reader attachment");
    proc_destroy(&caller);
}
static int64_t prepare(const struct boot_info *bi, const char *name)
{
    const struct boot_module *m = module(bi, name);
    struct elf_image image;
    int error;
    require(m && !load_elf(&caller, name, phys_to_virt(m->start), m->end - m->start,
                         &image, &error) && caller.abi == 2, "unchanged ABI v2 fixture");
    require(caller.pid > last_pid, "same process record receives a new PID");
    last_pid = caller.pid;
    const struct file_grant g = {0, FILE_RIGHTS, "dup/x", 5};
    require(!file_grants_install(&caller, &g, 1), "explicit non-root grant");
    require(!proc_create(&controller, "n36-control", exit_code, sizeof(exit_code)), "barrier controller");
    int64_t handle, peer;
    require(!channel_create(&caller, HANDLE_RECV, &controller, HANDLE_SEND, &handle, &peer) &&
            handle == 1 && !proc_start(&caller) && blocked(caller.tid) &&
            caller.syscalls == 2 && !file_process_busy(&caller), "observed pre-read channel barrier");
    require(caller.tid > last_tid, "new thread identity on reused process record");
    last_tid = caller.tid;
    capture(&sample);
    unsigned matches = 0;
    for (unsigned i = 0; i < THREAD_MAX; i++)
        if (sample.threads[i].tid == caller.tid) {
            expected_thread = sample.threads[i];
            matches++;
        }
    require(matches == 1 && expected_thread.record &&
            expected_thread.process == (uint64_t)&caller &&
            expected_thread.stack_owner == expected_thread.record,
            "exact pre-read thread identity anchors later ACTIVE checks");
    unchanged_observer();
    return peer;
}
static void release(int64_t peer)
{
    require(!channel_send(&controller, peer, 0, 0) && !handle_close(&controller, peer),
            "release existing channel barrier");
}
static uint64_t active(int real)
{
    capture(&sample);
    require(processes(&sample) == 2 && sample.file.refs == 2 && sample.file.active == 1 &&
            caller.syscalls == 3 && file_process_busy(&caller), "ACTIVE owners remain charged inside read syscall");
    unsigned ns = 0, nt = 0, busy = 0, loading = 0, requests = 0;
    uint64_t ticket = 0;
    for (unsigned i = 0; i < PROC_TABLE; i++) {
        const struct observe_session *s = &sample.file.sessions[i];
        if (s->owner) {
            ns++;
            require(s->owner == (uint64_t)&caller && s->pid == caller.pid &&
                    s->tid == caller.tid && s->active && s->initial[0],
                    "session identity matches live process and thread");
            unsigned nodes = 0;
            for (unsigned j = 0; j < FILE_MAX; j++) nodes += s->nodes[j].used == 2;
            require(nodes == 2, "exact grant and open node");
        }
    }
    for (unsigned i = 0; i < IO_SCOPE_TICKETS; i++) {
        const struct observe_ticket *t = &sample.tickets[i];
        if (t->id) {
            nt++;
            ticket = t->id;
            require(t->pid == caller.pid && t->tid == caller.tid && t->scope && t->guard &&
                    t->request == (unsigned)real && !t->abandoned, "scope/ticket identity and live reference");
        }
    }
    require(active_identities(&sample), "exact ACTIVE process/thread/stack identities");
    active_negative_checks();
    for (unsigned i = 0; i < EXT2_MAX_FS; i++) busy += sample.readers[i].busy != 0;
    for (unsigned i = 0; i < BLK_CACHE_LINES; i++) loading += sample.block.lines[i].state == BLK_LINE_LOADING;
    for (unsigned i = 0; i < VBLK_SLOTS; i++) {
        const struct observe_slot *s = &sample.driver.slots[i];
        requests += s->inflight != 0;
        if (s->inflight) {
            require(sample.driver.descriptors_available, "active request has initialized descriptor backing");
            require(s->tid == caller.tid && s->ticket == ticket && s->owner &&
                    !s->abandoned, "device slot retains matching owner and scalar ticket");
            for (unsigned j = 3*i; j < 3*i+3; j++)
                require(vblk_dma_ok(sample.driver.descriptors[j].address,
                                    sample.driver.descriptors[j].length), "published descriptor stays in DMA pool");
        }
    }
    require(ns == 1 && nt == 1 && busy == 1 && loading == 1 &&
            requests == (unsigned)real, "one observed reader/load/scope; no invented RAM DMA");
    require(pmm_check_in(pmm_kernel()) == PMM_OK &&
            pmm_frame_state_in(pmm_kernel(), caller.root) == PMM_FRAME_ALLOCATED,
            "pending process root is still allocated");
    for (unsigned i = 0; i < caller.npages; i++)
        require(pmm_frame_state_in(pmm_kernel(), caller.page_pa[i]) == PMM_FRAME_ALLOCATED,
                "pending user pages remain allocated");
    require(!file_audit() && !io_scope_audit() && !proc_audit() && !sched_audit(),
            "live-owner audits hold during pending I/O");
    unchanged_observer();
    return ticket;
}
static void finish(int result)
{
    require(proc_wait(&caller) == PROC_EXITED && caller.exit_status == result,
            "trusted kernel checks exact fixture result");
    proc_destroy(&caller);
    proc_destroy(&controller);
}
static void real_pending(const struct boot_info *bi, int cancelling)
{
    int64_t peer = prepare(bi, "file-reader");
    require(!blk_invalidate(vblk_info()->blk_id), "cold real-device operation");
    vblk_test_skip_notify(1);
    vblk_test_poll_limit(cancelling ? 8192 : 0);
    vblk_test_yield_polls(32);
    uint64_t yields = vblk_info()->yields;
    release(peer);
    if (vblk_info()->mode == VBLK_MODE_IRQ) {
        require(blocked(caller.tid), "observed IRQ wait");
    } else {
        for (unsigned i = 0; i < 100 && (!file_process_busy(&caller) ||
             vblk_info()->yields == yields); i++) thread_yield();
        require(thread_state(caller.tid) == THREAD_RUNNABLE &&
                file_process_busy(&caller) && vblk_info()->yields > yields,
                "observed bounded polling yield; no interruptible-wait assumption");
    }
}
static void ram_info(void *ctx, struct blk_info *out)
{
    (void)ctx;
    *out = (struct blk_info){BLK_DEV_READY, fixture_bytes/512, 8, BLK_CAP_READ, 1};
}
static int ram_read(void *ctx, uint64_t lba, uint32_t n, void *out, uint64_t cap)
{
    (void)ctx;
    backend_reads++;
    if (hold) {
        hold = 0;
        if (io_scope_wait(&gate, irq_ticks()+50) == THREAD_INTERRUPTED) return BLK_EINTR;
    }
    if (fail_reads) return BLK_EIO;
    if (!n || n > 8 || cap < n*512u || lba >= fixture_bytes/512 ||
        n > fixture_bytes/512-lba) return BLK_EINVAL;
    memcpy(out, fixture+lba*512, n*512u);
    return BLK_OK;
}
static const struct blk_ops ram_ops = {"n36-existing-EIO-model", ram_info, ram_read, NULL};

int selftest_accounting(const struct boot_info *bi)
{
    const struct boot_module *m = module(bi, "ext2-1k.img");
    require(m && vblk_info()->state == VBLK_READY && !vblk_info()->quarantined,
            "required immutable fixture and supported real driver");
    fixture = phys_to_virt(m->start);
    fixture_bytes = m->end-m->start;
    capture(&base);
    require(!processes(&base) && !base.file.fs && !ext2_open_count() &&
            pmm_kernel()->frames == PMM_MAX_FRAMES, "declared pre-smoke baseline");
    memcpy(allocation, pmm_kernel()->alloc, sizeof(allocation));
    quiet(base.block.devices_count, 0);
    observer_negative_checks();

    /* SMOKE-01: frozen ring-3 fixture checks distinct exact data and short length. */
    attach(vblk_info()->blk_id);
    real_pending(bi, 0);
    active(1);
    vblk_test_notify();
    finish(0);
    vblk_test_poll_limit(0);
    vblk_test_yield_polls(VBLK_YIELD_POLLS);
    quiet(base.block.devices_count, 1);
    require(!file_volume_detach(), "successful reader detach");
    serial_puts("accounting: SMOKE-01 PASS\r\n");

    /* SMOKE-02: same BLK_EIO backend/fallback mechanism as N3.5 file tests. */
    require(!blk_register("n36-ram", &ram_ops, NULL, &ram_device), "register immutable RAM error model");
    attach(ram_device);
    int64_t peer = prepare(bi, "file-error");
    require(!blk_invalidate(ram_device), "cold modeled-media read");
    hold = fail_reads = 1;
    uint64_t before = backend_reads;
    release(peer);
    require(blocked(caller.tid) && backend_reads > before, "observed actual backend I/O before EIO");
    active(0);
    thread_wake_all(&gate);
    finish(FILE_EIO); /* frozen error.elf verifies all 4096 sentinel bytes unchanged */
    require(backend_reads >= before+2, "whole-line error reached existing sector fallback");
    quiet(base.block.devices_count+1, 1);
    fail_reads = 0;
    peer = prepare(bi, "file-reader");
    release(peer);
    finish(0);
    quiet(base.block.devices_count+1, 1);
    require(!file_volume_detach() && !blk_unregister(ram_device), "release error-model reader and registry");
    serial_puts("accounting: SMOKE-02 PASS\r\n");

    /* SMOKE-03: actual process kill after the observed real-driver wait. */
    attach(vblk_info()->blk_id);
    real_pending(bi, 1);
    uint64_t ticket = active(1), pid = caller.pid, tid = caller.tid;
    uint64_t retired = vblk_info()->abandoned_completed;
    int irq = vblk_info()->mode == VBLK_MODE_IRQ;
    proc_kill(&caller, "N3.6 observed I/O cancellation");
    require(proc_wait(&caller) == PROC_KILLED && caller.trap.rax == SYS_FILE_READ_AT &&
            caller.syscalls == 3 && !file_process_busy(&caller), "kill unwinds without user read result");
    proc_destroy(&caller);
    proc_destroy(&controller);
    capture(&sample);
    if (irq) {
        unsigned debt = 0, abandoned = 0;
        require(!processes(&sample) && !file_session_count() && !sample.file.active &&
                !sample.file.refs && thread_state(tid) == THREAD_FREE, "DETACHED caller pages/session/stack reclaimed");
        for (unsigned i = 0; i < IO_SCOPE_TICKETS; i++) {
            const struct observe_ticket *t = &sample.tickets[i];
            if (t->id) {
                debt++;
                require(t->id == ticket && t->pid == pid && !t->tid && !t->scope &&
                        !t->guard && t->request && t->abandoned, "detached ticket retains scalar debt only");
            }
        }
        for (unsigned i = 0; i < VBLK_SLOTS; i++) {
            const struct observe_slot *s = &sample.driver.slots[i];
            if (s->inflight) {
                abandoned++;
                require(s->ticket == ticket && s->abandoned && !s->owner &&
                        !s->tid && !s->head && !s->tail, "abandoned DMA has no dead owner");
            }
        }
        require(debt == 1 && abandoned == 1, "exact detached request and debt");
        frames();
        unchanged_observer();
        /* Existing notify; observer never calls service/collect. */
        vblk_test_notify();
        for (unsigned i = 0; i < 30 && vblk_abandoned(); i++) thread_sleep(1);
        require(!vblk_abandoned() && !io_scope_count() &&
                vblk_info()->abandoned_completed == retired+1 &&
                vblk_info()->state == VBLK_READY, "deferred completion retires exactly once without file syscall");
    } else {
        require(!io_scope_count() && !vblk_abandoned() && !vblk_inflight() &&
                vblk_test_last_spins() <= 8192 && vblk_info()->state == VBLK_BROKEN,
                "polling kill finishes at finite inner bound; no orphan");
        serial_puts("accounting: NOT APPLICABLE: IRQ DETACHED state in polling mode\r\n");
    }
    vblk_test_poll_limit(0);
    vblk_test_yield_polls(VBLK_YIELD_POLLS);
    quiet(base.block.devices_count, 1);
    require(!file_volume_detach(), "cancelled binding reader released");
    if (!irq)
        require(vblk_reinit() == VBLK_READY, "existing supported polling-timeout recovery");
    quiet(base.block.devices_count, 0);
    serial_puts("accounting: SMOKE-03 PASS\r\n");
    serial_puts("accounting: checks ");
    serial_dec(checks);
    serial_puts("; exact quiescent accounting PASS\r\n");
    return 0;
}
