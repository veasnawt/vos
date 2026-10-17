/*
 * N2.3 multi-process tests (VOS_TEST_CASE=mp): separately built VOS
 * executables, delivered by GRUB (tests/x86_64/mp.grub.cfg), loaded with the
 * real ELF loader and run as concurrent, preemptively scheduled processes.
 * Every verdict comes from the kernel's records of each process (state, exit
 * status, kill reason, ticks it ran, preemptions, where its trap frames
 * were, which frames it owned); program output is never trusted.
 */
#include "selftest.h"
#include "console.h"
#include "cpu.h"
#include "exception.h"
#include "irq.h"
#include "lib.h"
#include "loader.h"
#include "memlayout.h"
#include "pmm.h"
#include "process.h"
#include "thread.h"

static int failures, cases;
static const struct boot_info *mbi;

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

/* What the kernel recorded about a finished process (copied before destroy). */
struct res {
    int state, status;
    const char *reason;
    uint64_t pid, ticks, preemptions, traps, kstack_top, root, last_kframe, vector, sys_ok;
    uint64_t frames[8];
    unsigned nframes;
};

static struct process *const P = selftest_procs;
static struct res R[PROC_TABLE];

static const struct boot_module *module(const char *name)
{
    for (uint32_t i = 0; i < mbi->module_count; i++)
        if (streq(mbi->modules[i].cmdline, name))
            return &mbi->modules[i];
    return NULL;
}

/* Load the GRUB module `mod` into p and start it. */
static int launch(struct process *p, const char *mod)
{
    const struct boot_module *m = module(mod);
    struct elf_image im;
    int eerr;

    if (!m || load_elf(p, mod, phys_to_virt(m->start), m->end - m->start, &im, &eerr) != PROC_OK)
        return 0;
    if (proc_start(p) == PROC_OK)
        return 1;
    proc_destroy(p);
    return 0;
}

static int collect(struct process *p, struct res *r)
{
    int st = proc_wait(p);

    memset(r, 0, sizeof(*r));
    r->state = st;
    r->status = p->exit_status;
    r->reason = p->reason;
    r->pid = p->pid;
    r->ticks = p->ticks;
    r->preemptions = p->preemptions;
    r->traps = p->traps;
    r->kstack_top = p->kstack_top;
    r->root = p->root;
    r->last_kframe = p->last_irq.kframe;
    r->vector = p->trap.vector;
    r->sys_ok = p->sys_ok;
    for (unsigned i = 0; i < p->npages && r->nframes < 8; i++)
        r->frames[r->nframes++] = p->page_pa[i];
    proc_destroy(p);
    return st;
}

static int killed_by_test(const struct res *r)
{
    return r->state == PROC_KILLED && streq(r->reason, "test kill");
}

static int no_shared_frame(const struct res *a, const struct res *b)
{
    for (unsigned i = 0; i < a->nframes; i++)
        for (unsigned j = 0; j < b->nframes; j++)
            if (a->frames[i] == b->frames[j])
                return 0;
    return 1;
}

static int own_stack(const struct res *r)
{
    return r->last_kframe + sizeof(struct exc_frame) == r->kstack_top;
}

/* Let processes P[0..n) run for `ticks`, then kill and collect them all. */
static void run_for(unsigned n, uint64_t ticks)
{
    thread_sleep(ticks);
    for (unsigned i = 0; i < n; i++)
        proc_kill(&P[i], "test kill");
    for (unsigned i = 0; i < n; i++)
        collect(&P[i], &R[i]);
}

static void report(const char *what, unsigned n)
{
    serial_puts("mp: ");
    serial_puts(what);
    serial_puts(":");
    for (unsigned i = 0; i < n; i++) {
        serial_puts(" [pid ");
        serial_dec(R[i].pid);
        serial_puts(" ran ");
        serial_dec(R[i].ticks);
        serial_puts(" ticks, ");
        serial_dec(R[i].preemptions);
        serial_puts(" preemptions]");
    }
    serial_puts("\r\n");
}

static int baseline(uint64_t f0, unsigned threads0)
{
    return pmm_free_count() == f0 && pmm_check_in(pmm_kernel()) == PMM_OK && proc_count() == 0 &&
           thread_count() == threads0 && thread_canaries_intact();
}

int selftest_mp(const struct boot_info *bi)
{
    uint64_t f0 = pmm_free_count();
    unsigned threads0 = thread_count();
    int ok;

    mbi = bi;
    failures = cases = 0;
    proc_test_tick_budget(400);         /* bounds every case; the kills end them much earlier */

    /* 1. Two programs, then three, progressing independently and isolated. */
    ok = launch(&P[0], "m-memtag1") && launch(&P[1], "m-memtag2");
    run_for(2, 20);
    expect(ok && killed_by_test(&R[0]) && killed_by_test(&R[1]) && R[0].ticks >= 4 && R[1].ticks >= 4 &&
           R[0].preemptions > 0 && R[1].preemptions > 0,
           "two programs writing the same user address run side by side; neither ever sees the other's data");
    expect(R[0].root != R[1].root && no_shared_frame(&R[0], &R[1]),
           "their address spaces have different roots and no user frame in common");
    report("memtag1 + memtag2", 2);

    ok = launch(&P[0], "m-memtag1") && launch(&P[1], "m-memtag2") && launch(&P[2], "m-irqregs");
    run_for(3, 30);
    expect(ok && killed_by_test(&R[0]) && killed_by_test(&R[1]) && killed_by_test(&R[2]) &&
           R[2].preemptions > 0, "three programs progress; irqregs keeps all registers and DF across preemption");
    expect(R[0].kstack_top != R[1].kstack_top && R[1].kstack_top != R[2].kstack_top &&
           R[0].kstack_top != R[2].kstack_top && own_stack(&R[0]) && own_stack(&R[1]) && own_stack(&R[2]),
           "every process trapped at the top of its own kernel stack (its TSS.RSP0)");
    report("memtag1 + memtag2 + irqregs", 3);
    expect(baseline(f0, threads0), "after two and three processes: frames, records and threads back to baseline");

    /* 2./3. Fairness: three endless loops. */
    ok = launch(&P[0], "m-spin") && launch(&P[1], "m-spin") && launch(&P[2], "m-spin");
    uint64_t t0 = irq_ticks();
    run_for(3, 60);
    uint64_t span = irq_ticks() - t0, lo = ~0ULL, hi = 0, sum = 0;
    for (int i = 0; i < 3; i++) {
        lo = R[i].ticks < lo ? R[i].ticks : lo;
        hi = R[i].ticks > hi ? R[i].ticks : hi;
        sum += R[i].ticks;
    }
    expect(ok && killed_by_test(&R[0]) && killed_by_test(&R[1]) && killed_by_test(&R[2]) && lo >= 15 &&
           hi - lo <= 2 * SCHED_SLICE_TICKS + 2 && span >= 60 && sum <= span + 2,
           "three endless loops share the CPU round robin (and only count the ticks they ran)");
    report("3 x spin (fairness)", 3);

    /* An endless loop beside normal programs: they finish; their system calls work. */
    ok = launch(&P[0], "m-spin") && launch(&P[1], "hello") && launch(&P[2], "m-syscalls");
    int sh = collect(&P[1], &R[1]), ss = collect(&P[2], &R[2]);
    int spinning = P[0].state == PROC_RUNNING;
    proc_kill(&P[0], "test kill");
    collect(&P[0], &R[0]);
    expect(ok && sh == PROC_EXITED && R[1].status == 0 && R[1].sys_ok == 1 && ss == PROC_EXITED &&
           R[2].status == 0 && spinning && killed_by_test(&R[0]) && R[0].preemptions > 0,
           "hello and the system-call program finish next to an endless loop, which runs on until killed");

    /* 7./8. A fault kills only its process; an exit leaves the others running. */
    ok = launch(&P[0], "m-spin") && launch(&P[1], "m-null") && launch(&P[2], "hello");
    int sn = collect(&P[1], &R[1]);
    sh = collect(&P[2], &R[2]);
    spinning = P[0].state == PROC_RUNNING;
    proc_kill(&P[0], "test kill");
    collect(&P[0], &R[0]);
    expect(ok && sn == PROC_KILLED && R[1].vector == 14 && sh == PROC_EXITED && R[2].status == 0 && spinning &&
           killed_by_test(&R[0]), "a page fault ends only the faulting process; the others go on");

    /* 10. Exhaustion: the process table is full at PROC_TABLE running processes. */
    unsigned n = 0;
    while (n < PROC_TABLE && launch(&P[n], "m-spin"))
        n++;
    static struct process extra;
    const struct boot_module *m = module("m-spin");
    struct elf_image im;
    int eerr, full = load_elf(&extra, "extra", phys_to_virt(m->start), m->end - m->start, &im, &eerr);
    thread_sleep(5);
    int all_running = 1;
    for (unsigned i = 0; i < n; i++)
        all_running &= P[i].state == PROC_RUNNING;
    run_for(n, 0);
    int all_killed = 1;
    for (unsigned i = 0; i < n; i++)
        all_killed &= killed_by_test(&R[i]);
    expect(n == PROC_TABLE && full == PROC_EBUSY && all_running && all_killed,
           "PROC_TABLE programs run at once; one more is refused (PROC_EBUSY); all are killed and collected");
    expect(baseline(f0, threads0), "after the full table: frames, records and threads back to baseline");

    /* 11./12. Repeated creation and destruction, then hello on its own. */
    int cycles = 1;
    for (int i = 0; i < 10; i++) {
        cycles &= launch(&P[0], "m-spin") && launch(&P[1], "hello");
        cycles &= collect(&P[1], &R[1]) == PROC_EXITED && R[1].status == 0;
        proc_kill(&P[0], "test kill");
        cycles &= collect(&P[0], &R[0]) == PROC_KILLED;
    }
    expect(cycles && baseline(f0, threads0), "10 rounds of hello beside an endless loop leak nothing");
    ok = launch(&P[0], "hello");
    expect(ok && collect(&P[0], &R[0]) == PROC_EXITED && R[0].status == 0 && R[0].preemptions == 0 &&
           baseline(f0, threads0), "hello alone afterwards: exits 0, never preempted, everything reclaimed");

    proc_test_tick_budget(PROC_TEST_TICK_BUDGET);
    serial_puts("selftest: mp ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}
