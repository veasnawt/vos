/*
 * N2.5 integration and stress tests (VOS_TEST_CASE=stress): mixed workloads
 * of separately built programs from GRUB modules (tests/x86_64/stress.grub.cfg)
 * running as concurrent processes, with several channels, kills, faults,
 * exhaustion and recovery. The scenarios are trusted kernel code; every
 * verdict comes from the kernel's own records, and the scheduler, process
 * and channel audits (sched_audit, proc_audit, channel_audit) run while the
 * processes are running or blocked, not only at the end. Every scenario is
 * bounded: programs that never end are killed by the kernel after a fixed
 * number of ticks, and the test tick budget backs that up.
 */
#include "selftest.h"
#include "channel.h"
#include "console.h"
#include "cpu.h"
#include "irq.h"
#include "lib.h"
#include "loader.h"
#include "memlayout.h"
#include "pmm.h"
#include "process.h"
#include "syscall.h"
#include "thread.h"

static int failures, cases;
static const struct boot_info *mbi;
static struct process *const P = selftest_procs;
static uint64_t f0;
static unsigned t0;

static void expect(int ok, const char *what)
{
    cases++;
    if (ok)
        return;
    failures++;
    serial_puts("selftest: FAIL: ");
    serial_puts(what);
    serial_puts("\r\n");
}

static int streq(const char *a, const char *b)
{
    return a && b && strlen(a) == strlen(b) && memcmp(a, b, strlen(a)) == 0;
}

/* What the kernel recorded about a finished process. */
struct res {
    int state, status;
    const char *reason;
    uint64_t vector, ticks, traps, preemptions, in, out;
    int hash_eq;
};
static struct res R[PROC_TABLE];

static int load(struct process *p, const char *mod)
{
    struct elf_image im;
    int eerr;

    for (uint32_t i = 0; i < mbi->module_count; i++)
        if (streq(mbi->modules[i].cmdline, mod))
            return load_elf(p, mod, phys_to_virt(mbi->modules[i].start),
                            mbi->modules[i].end - mbi->modules[i].start, &im, &eerr) == PROC_OK;
    return 0;
}

static int launch(struct process *p, const char *mod)
{
    return load(p, mod) && proc_start(p) == PROC_OK;
}

static int wire(struct process *a, struct process *b)
{
    int64_t ha, hb;

    return channel_create(a, HANDLE_RIGHTS, b, HANDLE_RIGHTS, &ha, &hb) == 0 && ha == 1 && hb == 1;
}

static void collect(unsigned i)
{
    struct process *p = &P[i];

    proc_wait(p);
    R[i].state = p->state;
    R[i].status = p->exit_status;
    R[i].reason = p->reason;
    R[i].vector = p->trap.vector;
    R[i].ticks = p->ticks;
    R[i].traps = p->traps;
    R[i].preemptions = p->preemptions;
    R[i].in = p->ipc_in;
    R[i].out = p->ipc_out;
    R[i].hash_eq = p->ipc_in_hash == p->ipc_out_hash;
    proc_destroy(p);
}

static void kill_collect(unsigned i)
{
    proc_kill(&P[i], "test kill");
    collect(i);
}

static int killed(unsigned i)
{
    return R[i].state == PROC_KILLED && streq(R[i].reason, "test kill");
}

static int exited(unsigned i, int status)
{
    return R[i].state == PROC_EXITED && R[i].status == status;
}

/* All three audits; prints the first broken invariant. */
static int audits(void)
{
    const char *why = sched_audit();

    if (!why)
        why = proc_audit();
    if (!why)
        why = channel_audit();
    if (why) {
        serial_puts("selftest:   audit: ");
        serial_puts(why);
        serial_puts("\r\n");
    }
    return why == NULL;
}

static int baseline(void)
{
    return pmm_free_count() == f0 && pmm_check_in(pmm_kernel()) == PMM_OK && proc_count() == 0 &&
           channel_count() == 0 && thread_count() == t0 && audits();
}

/* ---- scenarios ---- */

/* CPU-bound processes, a register checker, isolated writers and an IPC pair, beside a sleeping kernel thread. */
static void mixed_load(void)
{
    static const char *const mod[7] = { "spin", "spin", "irqregs", "memtag1", "memtag2", "ipc_ping", "ipc_echo" };
    int ok = 1;
    uint64_t late = 0, mid_ok = 1, wake_late0;

    for (unsigned i = 0; i < 5; i++)
        ok &= launch(&P[i], mod[i]);
    ok &= load(&P[5], mod[5]) && load(&P[6], mod[6]) && wire(&P[5], &P[6]) && proc_start(&P[6]) == PROC_OK &&
          proc_start(&P[5]) == PROC_OK;
    wake_late0 = sched_stats()->max_wake_late;
    for (int k = 0; k < 5; k++) {           /* a kernel thread sleeping beside six busy processes */
        uint64_t s = irq_ticks();
        thread_sleep(4);
        uint64_t l = irq_ticks() - s - 4;
        late = l > late ? l : late;
        mid_ok &= audits();
    }
    collect(5);
    collect(6);
    for (unsigned i = 0; i < 5; i++)
        kill_collect(i);
    expect(ok && mid_ok, "mixed load: seven processes started; audits hold while they run");
    expect(exited(5, 0) && exited(6, 10) && R[5].in == 10 && R[5].out == 10 && R[6].in == 10 && R[6].out == 10 &&
           R[5].hash_eq && R[6].hash_eq, "mixed load: ping/echo completes, every message delivered in order");
    expect(killed(0) && killed(1) && killed(2) && killed(3) && killed(4) && R[2].preemptions > 0,
           "mixed load: irqregs and both memtags never failed (only the kernel ended them)");
    expect(R[0].ticks >= 3 && R[1].ticks >= 3 && R[2].ticks >= 3 && R[3].ticks >= 3 && R[4].ticks >= 3,
           "mixed load: every CPU-bound process made progress");
    /*
     * A sleeper becomes runnable on the tick its deadline passes (max_wake_late,
     * recorded by the scheduler), then waits its turn behind the runnable
     * processes: at most one round of the run queue, 7 processes x one slice.
     */
    expect(sched_stats()->max_wake_late <= 1 && wake_late0 <= 1,
           "mixed load: the sleeping kernel thread became runnable on the tick of its deadline");
    expect(late <= 7 * SCHED_SLICE_TICKS + 2,
           "mixed load: it then ran within one round of the run queue (round-robin bound)");
    serial_puts("stress: mixed load: kernel sleeper at most ");
    serial_dec(late);
    serial_puts(" ticks late; spin ticks ");
    serial_dec(R[0].ticks);
    serial_puts("/");
    serial_dec(R[1].ticks);
    serial_puts("\r\n");
    expect(baseline(), "mixed load: everything back to baseline");
}

/* Three independent channels, three start orders, plus an endless loop. */
static void three_channels(void)
{
    int ok = 1;

    for (unsigned i = 0; i < 6; i += 2)
        ok &= load(&P[i], "ipc_ping") && load(&P[i + 1], "ipc_echo") && wire(&P[i], &P[i + 1]);
    ok &= launch(&P[6], "spin");
    ok &= proc_start(&P[1]) == PROC_OK;     /* pair 1: receiver first */
    thread_sleep(2);
    ok &= proc_start(&P[0]) == PROC_OK;
    ok &= proc_start(&P[2]) == PROC_OK;     /* pair 2: sender first */
    ok &= proc_start(&P[3]) == PROC_OK;
    ok &= proc_start(&P[4]) == PROC_OK && proc_start(&P[5]) == PROC_OK;   /* pair 3: together */
    int mid = audits();
    for (unsigned i = 0; i < 6; i++)
        collect(i);
    kill_collect(6);
    int pairs = 1;
    for (unsigned i = 0; i < 6; i += 2)
        pairs &= exited(i, 0) && exited(i + 1, 10) && R[i].in == 10 && R[i + 1].in == 10 && R[i].hash_eq &&
                 R[i + 1].hash_eq;
    expect(ok && mid && pairs && killed(6), "three channels at once, three start orders: every pair completes in order");
    expect(baseline(), "three channels: everything back to baseline");
}

/* A process dies in the middle of a conversation; its peer notices and ends cleanly. */
static void death_during_ipc(void)
{
    int ok = load(&P[0], "ipc_chat") && load(&P[1], "ipc_echo") && wire(&P[0], &P[1]) &&
             load(&P[2], "ipc_chat") && load(&P[3], "ipc_echo") && wire(&P[2], &P[3]) &&
             launch(&P[4], "spin") && launch(&P[5], "null");
    for (unsigned i = 0; i < 4; i++)
        ok &= proc_start(&P[i]) == PROC_OK;
    thread_sleep(10);
    int talking = P[0].state == PROC_RUNNING && P[1].state == PROC_RUNNING && P[2].state == PROC_RUNNING &&
                  P[3].state == PROC_RUNNING && audits();
    kill_collect(0);                        /* the talker dies: its echo gets EPIPE */
    kill_collect(3);                        /* the echo dies: its talker gets EPIPE */
    collect(1);
    collect(2);
    collect(5);
    kill_collect(4);
    expect(ok && talking, "two conversations run beside an endless loop and a faulting process");
    expect(killed(0) && R[1].state == PROC_EXITED && R[1].status > 0 && R[1].in >= (uint64_t)R[1].status,
           "the talker killed mid-conversation: its echo sees EPIPE and exits with the count it echoed");
    expect(killed(3) && R[2].state == PROC_EXITED && (R[2].status == 100 || R[2].status == 101),
           "the echo killed mid-conversation: its talker sees EPIPE (exit 100/101), not a wrong message");
    expect(R[5].state == PROC_KILLED && R[5].vector == 14 && killed(4), "the faulting process died alone");
    expect(baseline(), "death during IPC: everything back to baseline");
}

/* A blocked receiver does not use the CPU while others spin. */
static void blocked_idle(void)
{
    int ok = load(&P[0], "ipc_wait") && load(&P[1], "spin") && wire(&P[0], &P[1]) &&
             proc_start(&P[0]) == PROC_OK && proc_start(&P[1]) == PROC_OK && launch(&P[2], "spin");
    thread_sleep(30);
    int blocked = thread_state(P[0].tid) == THREAD_BLOCKED && audits();
    uint64_t wait_ticks = P[0].ticks, wait_traps = P[0].traps;
    for (unsigned i = 0; i < 3; i++)
        kill_collect(i);
    expect(ok && blocked && wait_ticks <= 1 && wait_traps <= 2 && killed(0) && R[1].ticks >= 10 && R[2].ticks >= 10,
           "a receiver blocked for 30 ticks used at most 1 tick while two endless loops shared the CPU");
    expect(baseline(), "blocked receiver: everything back to baseline");
}

/* The process table full, then recovery. */
static void process_pool(void)
{
    static struct process extra;
    unsigned n = 0;

    while (n < PROC_TABLE && launch(&P[n], "spin"))
        n++;
    int full = load(&extra, "hello") ? PROC_OK : (proc_count() == PROC_TABLE ? PROC_EBUSY : -99);
    int mid = audits();
    kill_collect(0);
    int again = launch(&P[0], "hello");
    collect(0);
    int hello = exited(0, 0);
    for (unsigned i = 1; i < n; i++)
        kill_collect(i);
    expect(n == PROC_TABLE && full == PROC_EBUSY && mid && again && hello,
           "8 processes fill the table; a 9th is refused; after one ends, hello runs in its place");
    expect(baseline(), "process pool: everything back to baseline");
}

/* The channel pool and a handle table full, then recovery and real traffic over a recycled channel. */
static void channel_and_handle_pools(void)
{
    int64_t ha, hb, first_a = 0, third_a = 0, third_b = 0;
    unsigned made = 0;
    int ok = load(&P[0], "ipc_echo") && load(&P[1], "ipc_echo") && load(&P[2], "ipc_echo") &&
             load(&P[3], "ipc_echo") && load(&P[4], "ipc_ping") && load(&P[5], "ipc_echo");

    while (channel_create(&P[0], HANDLE_RIGHTS, &P[1], HANDLE_RIGHTS, &ha, &hb) == 0) {
        if (made == 0)
            first_a = ha;
        if (made == 2) {
            third_a = ha;
            third_b = hb;
        }
        made++;
    }
    int handles_full = made == HANDLE_MAX;
    while (channel_create(&P[2], HANDLE_RIGHTS, &P[3], HANDLE_RIGHTS, &ha, &hb) == 0)
        made++;
    uint64_t fbefore = pmm_free_count();
    int pool_full = made == CHANNEL_MAX && channel_create(&P[4], HANDLE_RIGHTS, &P[5], HANDLE_RIGHTS, &ha, &hb) ==
                    -VOS_ENOMEM && pmm_free_count() == fbefore && audits();
    /* Recovery: close one channel, the pool and the handle slots work again with new generations. */
    int closed = handle_close(&P[0], third_a) == 0 && handle_close(&P[1], third_b) == 0 && channel_count() == 15;
    int recycled = channel_create(&P[0], HANDLE_RIGHTS, &P[1], HANDLE_RIGHTS, &ha, &hb) == 0 && (ha & 0xFF) == 3 &&
                   ha != third_a && channel_send(&P[0], third_a, USER_STACK_START, 1) == -VOS_EBADHANDLE;
    int closed2 = handle_close(&P[0], first_a) == 0 && channel_count() == 16;   /* half-closed: still in use */
    int still_full = channel_create(&P[4], HANDLE_RIGHTS, &P[5], HANDLE_RIGHTS, &ha, &hb) == -VOS_ENOMEM;
    proc_destroy(&P[1]);                    /* its 8 ends close: the half-closed channel is freed, 7 stay half-open */
    int freed = channel_count() == 15;
    int works = wire(&P[4], &P[5]) && proc_start(&P[5]) == PROC_OK && proc_start(&P[4]) == PROC_OK;
    collect(4);
    collect(5);
    proc_destroy(&P[0]);
    proc_destroy(&P[2]);
    proc_destroy(&P[3]);
    expect(ok && handles_full && pool_full, "8 handles fill a process's table; 16 channels fill the pool; "
           "the next creation fails with ENOMEM and changes nothing");
    expect(closed && recycled && closed2 && still_full && freed,
           "closing both ends frees a channel; a reused slot gets a new handle value; a half-closed channel "
           "still counts; destroying a process closes its ends and frees what it fully closes");
    expect(works && exited(4, 0) && exited(5, 10), "after recovery a new channel carries real traffic");
    expect(baseline(), "pools: everything back to baseline");
}

/* Fairness under contention: four endless loops beside IPC traffic. */
static void fairness(void)
{
    int ok = 1;

    for (unsigned i = 0; i < 4; i++)
        ok &= launch(&P[i], "spin");
    ok &= load(&P[4], "ipc_chat") && load(&P[5], "ipc_echo") && wire(&P[4], &P[5]) &&
          proc_start(&P[4]) == PROC_OK && proc_start(&P[5]) == PROC_OK;
    uint64_t start = irq_ticks(), snap[6];
    for (unsigned i = 0; i < 6; i++)
        snap[i] = P[i].ticks;           /* ticks before the window (started earlier) */
    thread_sleep(60);
    uint64_t span = irq_ticks() - start;
    for (unsigned i = 0; i < 6; i++)    /* the kernel's counts at the end of the window */
        snap[i] = P[i].ticks - snap[i];
    for (unsigned i = 0; i < 6; i++)    /* then end them all: killed ones may still run until scheduled */
        proc_kill(&P[i], "test kill");
    for (unsigned i = 0; i < 6; i++)
        collect(i);
    uint64_t lo = ~0ULL, hi = 0, sum = 0;
    for (unsigned i = 0; i < 4; i++) {
        lo = snap[i] < lo ? snap[i] : lo;
        hi = snap[i] > hi ? snap[i] : hi;
    }
    for (unsigned i = 0; i < 6; i++)
        sum += snap[i];
    serial_puts("stress: fairness over ");
    serial_dec(span);
    serial_puts(" ticks: spins ");
    for (unsigned i = 0; i < 4; i++) {
        serial_dec(snap[i]);
        serial_puts(i < 3 ? "/" : ", chat/echo ");
    }
    serial_dec(snap[4]);
    serial_puts("/");
    serial_dec(snap[5]);
    serial_puts("\r\n");
    expect(ok && killed(0) && killed(4) && lo >= 8 && hi - lo <= 2 * SCHED_SLICE_TICKS + 2 && sum <= span + 2,
           "four endless loops share the CPU fairly beside an IPC pair; no tick counted twice");
    expect(baseline(), "fairness: everything back to baseline");
}

/* Repeated creation, destruction and failure. */
static void churn(void)
{
    int ok = 1;

    for (int round = 0; round < 12; round++) {
        ok &= load(&P[0], "ipc_ping") && load(&P[1], "ipc_echo") && wire(&P[0], &P[1]) &&
              proc_start(&P[1]) == PROC_OK && proc_start(&P[0]) == PROC_OK;
        ok &= load(&P[2], "ipc_chat") && load(&P[3], "ipc_echo") && wire(&P[2], &P[3]) &&
              proc_start(&P[2]) == PROC_OK && proc_start(&P[3]) == PROC_OK;
        ok &= launch(&P[4], "null");
        collect(0);
        collect(1);
        collect(4);
        kill_collect(2);
        collect(3);
        ok &= exited(0, 0) && exited(1, 10) && R[4].vector == 14 && killed(2) && R[3].state == PROC_EXITED;
        /* A load that runs out of frames part-way leaves nothing behind. */
        proc_test_frame_budget(round % 5);
        int r = load(&P[5], "hello");
        proc_test_frame_budget(-1);
        if (r)
            proc_destroy(&P[5]);
        ok &= baseline();
    }
    expect(ok, "12 rounds of two conversations, a fault, a kill and a failed load: baseline after each");
}

int selftest_stress(const struct boot_info *bi)
{
    mbi = bi;
    failures = cases = 0;
    f0 = pmm_free_count();
    t0 = thread_count();
    proc_test_tick_budget(600);             /* bounds every program; the kills end them much earlier */
    expect(baseline(), "stress: starting from a consistent state");
    mixed_load();
    three_channels();
    death_during_ipc();
    blocked_idle();
    process_pool();
    channel_and_handle_pools();
    fairness();
    churn();
    proc_test_tick_budget(PROC_TEST_TICK_BUDGET);
    serial_puts("selftest: stress ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}
