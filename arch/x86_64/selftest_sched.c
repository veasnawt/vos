/*
 * Kernel threads and scheduler (N2.2), VOS_TEST only. Every verdict is the
 * kernel's own: the threads under test are trusted kernel code that record
 * what they observed in kernel memory, and the boot thread checks it.
 */
#include "selftest.h"
#include "bootinfo.h"
#include "console.h"
#include "cpu.h"
#include "exception.h"
#include "gdt.h"
#include "irq.h"
#include "kmap.h"
#include "lib.h"
#include "memlayout.h"
#include "panic.h"
#include "thread.h"
#include "vmm.h"

static int failures, cases;

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

static int streq_sched(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* ---- records and guarded stacks ---- */

static void test_stacks(void)
{
    uint64_t g[THREAD_STACKS], kg[KMAP_MAX_GUARDS];
    unsigned n = thread_guard_pages(g, THREAD_STACKS), nk = kmap_guard_pages(kg, KMAP_MAX_GUARDS);
    unsigned unmapped = 0, rw = 0, listed = 0, distinct = 1;
    struct vmm_mapping m;

    for (unsigned i = 0; i < n; i++) {
        unmapped += vmm_lookup(vmm_kernel_root(), g[i], &m) == VMM_ENOTMAPPED &&
                    vmm_lookup(vmm_kernel_root(), g[i] + THREAD_GUARD_SIZE - 8, &m) == VMM_ENOTMAPPED;
        /* the stack right above: 16 KiB, supervisor RW NX, 4 KiB pages */
        int ok = 1;
        for (uint64_t va = g[i] + THREAD_GUARD_SIZE; va < g[i] + THREAD_GUARD_SIZE + THREAD_STACK_SIZE; va += PAGE_SIZE)
            ok &= vmm_lookup(vmm_kernel_root(), va, &m) == VMM_OK && m.flags == VMM_WRITE &&
                  m.page_size == PAGE_SIZE;
        rw += ok;
        for (unsigned k = 0; k < nk; k++)
            listed += kg[k] == g[i];
        for (unsigned j = 0; j < i; j++)
            distinct &= g[j] != g[i] && (g[i] > g[j] ? g[i] - g[j] : g[j] - g[i]) >=
                                            THREAD_GUARD_SIZE + THREAD_STACK_SIZE;
        distinct &= g[i] % PAGE_SIZE == 0;
    }
    expect(n == THREAD_STACKS && unmapped == n && listed == n,
           "every thread stack has an unmapped guard page, listed in the kernel map's guards");
    expect(rw == n && distinct, "thread stacks: 16 KiB each, page aligned, disjoint, supervisor RW NX");

    struct thread *b = thread_current();
    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    expect(b && thread_is_boot() && b->id == 1 && b->state == THREAD_RUNNING && b->stack == -1 &&
           rsp > b->stack_lo && rsp <= b->stack_hi, "the boot code runs as thread 1 on the boot stack");
    expect(thread_canaries_intact() && gdt_tss_rsp0() > KERNEL_VMA &&
           !(gdt_tss_rsp0() > g[0] && gdt_tss_rsp0() <= g[n - 1] + THREAD_GUARD_SIZE + THREAD_STACK_SIZE),
           "canaries intact; TSS.RSP0 is the dedicated entry stack, never a thread stack");
}

/* ---- creation, switching, yield, exit ---- */

uint64_t yield_check_callee_saved(uint64_t seed, uint64_t rounds);  /* user_progs.S */

/* Yield until every listed thread is gone (reaped), at most max times. */
static int wait_gone(const int64_t *ids, unsigned n, unsigned max)
{
    for (unsigned k = 0; k < max; k++) {
        unsigned alive = 0;

        for (unsigned i = 0; i < n; i++)
            alive += ids[i] > 0 && thread_state((uint64_t)ids[i]) != THREAD_FREE;
        if (!alive)
            return 1;
        thread_yield();
    }
    return 0;
}

static char order_log[32];
static unsigned order_n;

static void order_worker(void *arg)
{
    for (int i = 0; i < 3; i++) {
        if (order_n < sizeof(order_log))
            order_log[order_n++] = (char)(uintptr_t)arg;
        thread_yield();
    }
}

static void test_order(void)
{
    int64_t id[3];
    uint64_t q[THREAD_MAX];

    order_n = 0;
    id[0] = thread_create("A", order_worker, (void *)'A');
    id[1] = thread_create("B", order_worker, (void *)'B');
    id[2] = thread_create("C", order_worker, (void *)'C');
    unsigned qn = sched_test_runq(q, THREAD_MAX);
    expect(id[0] > 0 && id[1] == id[0] + 1 && id[2] == id[1] + 1, "thread IDs unique and increasing");
    expect(qn == 3 && q[0] == (uint64_t)id[0] && q[1] == (uint64_t)id[1] && q[2] == (uint64_t)id[2],
           "new threads join the run queue in creation order");
    expect(wait_gone(id, 3, 50) && order_n == 9 && memcmp(order_log, "ABCABCABC", 9) == 0,
           "round robin: three yielding threads alternate A B C A B C A B C");
}

static uint64_t reg_diff[2];

static void reg_worker(void *arg)
{
    uintptr_t k = (uintptr_t)arg;

    reg_diff[k] = yield_check_callee_saved(k ? 0xA5A5A5A55A5A5A00ULL : 0x0123456789ABCD00ULL, 20);
}

static void test_registers(void)
{
    int64_t id[2];

    reg_diff[0] = reg_diff[1] = ~0ULL;
    id[0] = thread_create("regs0", reg_worker, (void *)0);
    id[1] = thread_create("regs1", reg_worker, (void *)1);
    uint64_t own = yield_check_callee_saved(0x5555AAAA0000FF00ULL, 25);
    expect(wait_gone(id, 2, 50) && reg_diff[0] == 0 && reg_diff[1] == 0 && own == 0,
           "rbx, rbp, r12-r15 of three threads survive 20+ switches each");
}

struct iso {
    uint64_t lo, hi, local;
    int ok;
};
static struct iso iso[3];

static void iso_worker(void *arg)
{
    struct iso *r = &iso[(uintptr_t)arg];
    volatile uint8_t buf[512];
    uint64_t rsp;

    for (unsigned i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)((uintptr_t)arg * 37 + i);
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    r->lo = thread_current()->stack_lo;
    r->hi = thread_current()->stack_hi;
    r->local = (uint64_t)buf;
    r->ok = rsp > r->lo && rsp < r->hi && r->local > r->lo && r->local < r->hi;
    for (int k = 0; k < 5; k++)
        thread_yield();
    for (unsigned i = 0; i < sizeof(buf); i++)
        r->ok &= buf[i] == (uint8_t)((uintptr_t)arg * 37 + i);
    r->ok &= thread_canaries_intact();
}

static void test_isolation(void)
{
    int64_t id[3];

    memset(iso, 0, sizeof(iso));
    for (uintptr_t i = 0; i < 3; i++)
        id[i] = thread_create("iso", iso_worker, (void *)i);
    int gone = wait_gone(id, 3, 50);
    int disjoint = iso[0].hi <= iso[1].lo || iso[1].hi <= iso[0].lo;
    disjoint &= (iso[1].hi <= iso[2].lo || iso[2].hi <= iso[1].lo) &&
                (iso[0].hi <= iso[2].lo || iso[2].hi <= iso[0].lo);
    expect(gone && iso[0].ok && iso[1].ok && iso[2].ok && disjoint,
           "each thread runs on its own stack; its locals survive while the others run");
}

static int64_t reap_seen_id;
static uint64_t reap_slot_lo;
static int reap_seen_free, reap_poisoned;

static void reap_victim(void *arg)
{
    (void)arg;
    reap_slot_lo = thread_current()->stack_lo;
}

static void reap_observer(void *arg)
{
    (void)arg;
    /* The victim ran and exited just before this thread's first instruction. */
    reap_seen_free = thread_state((uint64_t)reap_seen_id) == THREAD_FREE;
    reap_poisoned = 1;
    for (uint64_t a = reap_slot_lo + 4096; a < reap_slot_lo + 4096 + 256; a++)
        reap_poisoned &= *(volatile uint8_t *)a == 0xDD;
}

static void test_exit_and_reap(void)
{
    const struct sched_stats *s = sched_stats();
    uint64_t reaped0 = s->reaped, exits0 = s->exits;
    unsigned count0 = thread_count();
    int64_t id[2];

    reap_seen_free = reap_poisoned = 0;
    id[0] = reap_seen_id = thread_create("victim", reap_victim, 0);
    id[1] = thread_create("observer", reap_observer, 0);
    expect(wait_gone(id, 2, 20) && reap_seen_free && reap_poisoned,
           "a dead thread is reaped by the next thread to run: record free, stack poisoned");
    expect(s->exits == exits0 + 2 && s->reaped == reaped0 + 2 && thread_count() == count0,
           "every exit is reaped; the pool is back to its baseline");
    expect(thread_state((uint64_t)id[0]) == THREAD_FREE && thread_test_get((uint64_t)id[0]) == NULL,
           "a reaped thread's ID is gone for good");
}

static void noop_worker(void *arg)
{
    (void)arg;
}

static void test_exhaustion(void)
{
    int64_t id[THREAD_MAX + 2];
    unsigned free0 = THREAD_MAX - thread_count(), made = 0;
    uint64_t failed0 = sched_stats()->create_failed;
    int64_t err = 0;

    for (unsigned i = 0; i < THREAD_MAX + 2; i++) {
        id[i] = thread_create("fill", noop_worker, 0);
        if (id[i] > 0)
            made++;
        else if (!err)
            err = id[i];
    }
    expect(made == free0 && err == THREAD_ENOMEM && sched_stats()->create_failed == failed0 + THREAD_MAX + 2 - made,
           "creation beyond the pool fails with THREAD_ENOMEM; nothing else changes");
    expect(thread_create("bad", 0, 0) == THREAD_EINVAL, "creation without an entry function is refused");
    int gone = wait_gone(id, THREAD_MAX + 2, 50);
    int64_t again = thread_create("again", noop_worker, 0);
    expect(gone && again > id[made - 1] && wait_gone(&again, 1, 10) && THREAD_MAX - thread_count() == free0,
           "slots return after reaping; IDs keep increasing (never reused)");
}

/* ---- idle thread, ring-0 timer interrupts, blocking, sleeping ---- */

extern char idle_after_hlt[];

/* Sleep (one tick at a time) until every listed thread is gone, at most max ticks. */
static int wait_gone_ticks(const int64_t *ids, unsigned n, unsigned max)
{
    for (unsigned k = 0; k < max; k++) {
        unsigned alive = 0;

        for (unsigned i = 0; i < n; i++)
            alive += ids[i] > 0 && thread_state((uint64_t)ids[i]) != THREAD_FREE;
        if (!alive)
            return 1;
        thread_sleep(1);
    }
    return 0;
}

static void test_idle(void)
{
    const struct sched_stats *s = sched_stats();
    uint64_t t0 = irq_ticks(), irqs0 = s->idle_irqs, halts0 = s->idle_halts;
    uint64_t rip, rflags, rsp, lo, hi;

    thread_sleep(5);                    /* nothing else can run: only the idle thread */
    sched_test_last_idle_irq(&rip, &rflags, &rsp, &lo, &hi);
    /*
     * Every tick of the sleep is an IRQ taken by the idle thread. A halt can
     * see more than one: the IRETQ back restores IF = 1, so an IRQ that became
     * pending meanwhile is taken just before the cli (RIP still after hlt).
     */
    int ok = irq_ticks() - t0 >= 5 && s->idle_irqs - irqs0 == irq_ticks() - t0 && s->idle_halts - halts0 >= 1;
    if (!ok) {
        serial_puts("selftest:   ticks ");
        serial_dec(irq_ticks() - t0);
        serial_puts(" idle irqs ");
        serial_dec(s->idle_irqs - irqs0);
        serial_puts(" halts ");
        serial_dec(s->idle_halts - halts0);
        serial_puts("\r\n");
    }
    expect(ok, "boot thread asleep 5 ticks: the idle thread halted and took every timer IRQ in ring 0");
    expect(rip == (uint64_t)idle_after_hlt && (rflags & (RFLAGS_IF | RFLAGS_IOPL)) == RFLAGS_IF &&
           rsp > lo && rsp <= hi, "idle IRQ frame: RIP after hlt, IF 1, IOPL 0, on the idle thread's own stack");
    expect(!(read_rflags() & RFLAGS_IF) && thread_is_boot() && irq_chip()->in_service() == 0,
           "back on the boot thread with IF=0; nothing left in service at the controller");
}

static struct wait_queue wq;
static uint64_t wake_log[4];
static unsigned wake_n;

static void waiter(void *arg)
{
    thread_block(&wq);
    if (wake_n < 4)
        wake_log[wake_n++] = (uint64_t)(uintptr_t)arg;
}

static int all_blocked(const int64_t *ids, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (thread_state((uint64_t)ids[i]) != THREAD_BLOCKED)
            return 0;
    return 1;
}

static void test_block_wake(void)
{
    int64_t id[3];
    uint64_t q[THREAD_MAX];

    memset(&wq, 0, sizeof(wq));
    wake_n = 0;
    for (uintptr_t i = 0; i < 3; i++)
        id[i] = thread_create("waiter", waiter, (void *)(i + 1));
    for (int k = 0; k < 10 && !all_blocked(id, 3); k++)
        thread_yield();
    expect(all_blocked(id, 3) && sched_test_runq(q, THREAD_MAX) == 0 && wake_n == 0,
           "blocked threads are off the run queue and do not run");
    for (int k = 0; k < 5; k++)
        thread_yield();
    expect(wake_n == 0, "yielding never schedules a blocked thread");
    expect(thread_wake_one(&wq) == 1 && thread_state((uint64_t)id[0]) == THREAD_RUNNABLE &&
           thread_state((uint64_t)id[1]) == THREAD_BLOCKED, "wake_one makes exactly the first waiter runnable");
    thread_yield();
    expect(wake_n == 1 && wake_log[0] == 1, "the woken thread runs at its turn");
    expect(thread_wake_all(&wq) == 2 && wait_gone(id, 3, 20) && wake_n == 3 && wake_log[1] == 2 &&
           wake_log[2] == 3, "wake_all wakes the rest in FIFO order");
    expect(thread_wake_one(&wq) == 0 && thread_wake_all(&wq) == 0, "waking an empty queue does nothing");
}

static struct wait_queue items_q;
static unsigned items, consumed;
static int consumer_saw_all;

static void consumer(void *arg)
{
    (void)arg;
    while (consumed < 5) {
        while (items == 0)              /* check, then block: no switch can come in between */
            thread_block(&items_q);
        items--;
        consumed++;
    }
    consumer_saw_all = 1;
}

static void producer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 5; i++) {
        items++;
        thread_wake_one(&items_q);
        thread_yield();
    }
}

static void test_producer_consumer(void)
{
    int64_t id[2];

    memset(&items_q, 0, sizeof(items_q));
    items = consumed = 0;
    consumer_saw_all = 0;
    items = 1;                          /* an item before anyone waits: the consumer must not block on it */
    id[0] = thread_create("consumer", consumer, 0);
    id[1] = thread_create("producer", producer, 0);
    expect(wait_gone(id, 2, 50) && consumer_saw_all && consumed == 5 && items == 1 &&
           items_q.head == NULL, "producer/consumer over a wait queue: no lost or extra wakeup");
}

static uint64_t slept_from, slept_to;
static int64_t last_worker_id;

static void sleeper(void *arg)
{
    (void)arg;
    slept_from = irq_ticks();
    thread_sleep(3);
    slept_to = irq_ticks();
}

static void last_worker(void *arg)
{
    (void)arg;
    for (int k = 0; k < 3; k++)
        thread_yield();
}

static void test_sleep_and_last_exit(void)
{
    const struct sched_stats *s = sched_stats();
    int64_t id = thread_create("sleeper", sleeper, 0);

    slept_from = slept_to = 0;
    expect(wait_gone_ticks(&id, 1, 20) && slept_to - slept_from >= 3,
           "a sleeping thread wakes after at least its ticks");

    uint64_t reaped0 = s->reaped, irqs0 = s->idle_irqs;
    last_worker_id = thread_create("last", last_worker, 0);
    thread_sleep(3);                    /* the worker is the last runnable thread; then only idle */
    expect(thread_state((uint64_t)last_worker_id) == THREAD_FREE && s->reaped == reaped0 + 1 &&
           s->idle_irqs > irqs0, "the last runnable thread exits: idle reaps it and runs until the sleeper wakes");
}

/* ---- N3.4: queue waits with a deadline ---- */

static struct wait_queue dq;
static unsigned dl_seq;

struct dl_waiter {
    uint64_t delay, from, to;
    int reason;
    unsigned order;                     /* 1 + how many waits ended before this one */
};
static struct dl_waiter dlw[3];

static void dl_worker(void *arg)
{
    struct dl_waiter *w = arg;

    w->from = irq_ticks();
    w->reason = thread_block_until(&dq, w->from + w->delay);
    w->to = irq_ticks();
    w->order = ++dl_seq;
}

static int64_t dl_start(unsigned i, uint64_t delay)
{
    int64_t id;

    memset(&dlw[i], 0, sizeof(dlw[i]));
    dlw[i].delay = delay;
    dlw[i].reason = -1;
    id = thread_create("dl-waiter", dl_worker, &dlw[i]);
    for (int k = 0; k < 10 && id > 0 && thread_state((uint64_t)id) != THREAD_BLOCKED; k++)
        thread_yield();
    return id > 0 && thread_state((uint64_t)id) == THREAD_BLOCKED ? id : -1;
}

static void test_deadlines(void)
{
    const struct sched_stats *s = sched_stats();
    uint64_t t0, el, to0 = s->timeouts, b0, sw0, halts0 = s->idle_halts;
    int64_t id[3];
    int r;

    memset(&dq, 0, sizeof(dq));
    /* Nothing else can run and nothing wakes the boot thread: only its deadline. */
    t0 = irq_ticks();
    r = thread_block_until(&dq, t0 + 5);
    el = irq_ticks() - t0;
    serial_puts("selftest: deadline wait of 5 ticks ended after ");
    serial_dec(el);
    serial_puts(" ticks\r\n");
    expect(r == THREAD_TIMEDOUT && el >= 5 && el < 50 && s->idle_halts > halts0 && dq.head == NULL &&
           dq.tail == NULL && s->timeouts == to0 + 1 && thread_current()->wake_tick == 0,
           "a queue wait nobody ends returns THREAD_TIMEDOUT at its deadline (idle halted meanwhile), off the queue");

    b0 = s->blocks;
    sw0 = thread_current()->switches_in;
    expect(thread_block_until(&dq, irq_ticks()) == THREAD_TIMEDOUT && thread_block_until(&dq, 1) == THREAD_TIMEDOUT &&
           s->blocks == b0 && thread_current()->switches_in == sw0 && dq.head == NULL,
           "a deadline already due returns THREAD_TIMEDOUT at once, never enqueued, no switch");

    /* Woken before the deadline: the deadline is cleared and never fires later. */
    to0 = s->timeouts;
    id[0] = dl_start(0, 20);
    struct thread *w = id[0] > 0 ? thread_test_get((uint64_t)id[0]) : NULL;
    int armed = w && w->wake_tick && w->waiting_on == &dq && sched_audit() == NULL;
    int woke = thread_wake_one(&dq) == 1 && w && w->wake_tick == 0 && w->waiting_on == NULL;
    thread_sleep(25);                   /* past the old deadline */
    expect(armed && woke && dlw[0].reason == THREAD_WOKEN && dlw[0].to - dlw[0].from < 20 &&
           s->timeouts == to0 && thread_state((uint64_t)id[0]) == THREAD_FREE,
           "a wakeup before the deadline returns THREAD_WOKEN; the old deadline never fires");

    /* Interrupted before the deadline. */
    id[0] = dl_start(0, 1000);
    expect(id[0] > 0 && thread_interrupt((uint64_t)id[0]) == 1 && dq.head == NULL && wait_gone(id, 1, 10) &&
           dlw[0].reason == THREAD_INTERRUPTED && s->timeouts == to0,
           "thread_interrupt ends a deadline wait with THREAD_INTERRUPTED and takes it off the queue");

    /*
     * Three waiters queued A, B, C with deadlines 30, 5 and 15 ticks: B times
     * out from the middle of the queue, then C from its tail, and A, still
     * waiting, is woken normally. The queue stays well formed throughout.
     */
    dl_seq = 0;
    id[0] = dl_start(0, 30);
    id[1] = dl_start(1, 5);
    id[2] = dl_start(2, 15);
    struct thread *a = thread_test_get((uint64_t)id[0]), *c = thread_test_get((uint64_t)id[2]);
    int three = id[0] > 0 && id[1] > 0 && id[2] > 0 && sched_audit() == NULL;
    for (unsigned k = 0; k < 40 && !dlw[1].order; k++)
        thread_sleep(1);
    int after_b = dlw[1].order == 1 && dq.head == a && a && a->next == c && dq.tail == c && sched_audit() == NULL;
    for (unsigned k = 0; k < 40 && !dlw[2].order; k++)
        thread_sleep(1);
    int after_c = dlw[2].order == 2 && dq.head == a && dq.tail == a && a && !a->next && sched_audit() == NULL;
    int a_woken = !dlw[0].order && thread_wake_one(&dq) == 1;
    wait_gone(id, 3, 20);
    expect(three && after_b && after_c && a_woken, "waits time out from the middle and the tail of a queue; it stays linked");
    expect(dlw[1].reason == THREAD_TIMEDOUT && dlw[1].to - dlw[1].from >= 5 && dlw[2].reason == THREAD_TIMEDOUT &&
           dlw[2].to - dlw[2].from >= 15 && dlw[0].reason == THREAD_WOKEN && dlw[0].order == 3 &&
           s->timeouts == to0 + 2 && dq.head == NULL && dq.tail == NULL,
           "each wait ended by its own deadline (5 then 15 ticks), the longest by a wakeup before its own");

    /* The audit rule exists: a runnable thread must not carry a wake tick. */
    id[0] = thread_create("dl-audit", noop_worker, 0);
    w = id[0] > 0 ? thread_test_get((uint64_t)id[0]) : NULL;
    const char *bad = NULL;
    if (w) {
        w->wake_tick = irq_ticks() + 100;
        bad = sched_audit();
        w->wake_tick = 0;
    }
    expect(bad && streq_sched(bad, "a thread that is not blocked has a wake tick") && wait_gone(id, 1, 10) &&
           sched_audit() == NULL, "the scheduler audit flags a runnable thread with a wake tick");
}

/*
 * The ring-0 IRQ check on crafted frames placed in the unused low part of
 * the idle thread's stack (the idle thread is switched out). Real frames
 * from correct code never fail it, so this shows each condition exists.
 */
static int idle_frame_refused(struct exc_frame *f, const struct exc_frame *good, uint64_t *field, uint64_t bad)
{
    *f = *good;
    *field = bad;
    int ok = sched_test_idle_frame_ok(f, 1);
    *f = *good;
    return !ok;
}

static void test_idle_frame_check(void)
{
    uint64_t rip, rflags, rsp, lo, hi;
    struct exc_frame good, *f;

    sched_test_last_idle_irq(&rip, &rflags, &rsp, &lo, &hi);
    f = (struct exc_frame *)(lo + 2048);
    memset(&good, 0, sizeof(good));
    good.vector = 0x20;
    good.cs = GDT_KERNEL_CS;
    good.ss = GDT_KERNEL_DS;
    good.rip = (uint64_t)idle_after_hlt;
    good.rflags = 0x202;
    good.rsp = lo + 4096;
    *f = good;
    expect(sched_test_idle_frame_ok(f, 1), "idle IRQ check accepts the idle thread halted after hlt");
    expect(!sched_test_idle_frame_ok(f, 0), "idle IRQ check refuses any thread but the idle thread");
    expect(idle_frame_refused(f, &good, &f->rip, (uint64_t)idle_after_hlt - 1) &&
           idle_frame_refused(f, &good, &f->rip, (uint64_t)idle_after_hlt + 1),
           "idle IRQ check refuses any other RIP");
    expect(idle_frame_refused(f, &good, &f->rflags, 0x2), "idle IRQ check refuses a frame with IF=0");
    expect(idle_frame_refused(f, &good, &f->rflags, 0x3202), "idle IRQ check refuses IOPL=3");
    expect(idle_frame_refused(f, &good, &f->rflags, 0x20202), "idle IRQ check refuses VM=1");
    expect(idle_frame_refused(f, &good, &f->cs, GDT_USER_CS) && idle_frame_refused(f, &good, &f->ss, GDT_USER_DS),
           "idle IRQ check refuses user selectors");
    expect(idle_frame_refused(f, &good, &f->rsp, hi + 8) && idle_frame_refused(f, &good, &f->rsp, lo),
           "idle IRQ check refuses a saved RSP outside the idle stack");
    struct exc_frame elsewhere = good;
    expect(!sched_test_idle_frame_ok(&elsewhere, 1), "idle IRQ check refuses a frame not on the idle stack");
    memset(f, 0, sizeof(*f));
}

int selftest_sched(void)
{
    failures = cases = 0;
    test_stacks();
    test_order();
    test_registers();
    test_isolation();
    test_exit_and_reap();
    test_exhaustion();
    test_idle();
    test_idle_frame_check();
    test_block_wake();
    test_producer_consumer();
    test_sleep_and_last_exit();
    test_deadlines();
    expect(thread_is_boot() && thread_canaries_intact() && !(read_rflags() & RFLAGS_IF),
           "back on the boot thread with IF=0 and every canary intact");
    expect(sched_audit() == NULL, "the scheduler audit holds after the scheduler tests (N2.5)");
    const struct sched_stats *s = sched_stats();
    serial_puts("selftest: scheduler: ");
    serial_dec(s->created);
    serial_puts(" threads created, ");
    serial_dec(s->switches);
    serial_puts(" switches, ");
    serial_dec(s->reaped);
    serial_puts(" reaped, idle halted ");
    serial_dec(s->idle_halts);
    serial_puts(" times and took ");
    serial_dec(s->idle_irqs);
    serial_puts(" timer IRQs in ring 0\r\n");
    serial_puts("selftest: sched ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}

/* ---- isolated cases: each ends in a panic or a CPU exception (exit status 35) ---- */

#if defined(VOS_TEST_CASE_thread_overflow) || defined(VOS_TEST_CASE_sched_deadlock)
#define SCHED_CASE 1
#endif

#ifdef VOS_TEST_CASE_thread_overflow
static uint64_t __attribute__((noinline)) recurse(uint64_t n)
{
    volatile uint8_t buf[256];

    buf[n & 255] = (uint8_t)n;
    if (n == ~0ULL)
        return 0;
    return recurse(n + 1) + buf[n & 255];
}

static void overflow_worker(void *arg)
{
    (void)arg;
    serial_puts("fault-test: unbounded recursion on a kernel thread's stack\r\n");
    serial_puts("fault-test: expect vec=8 err=0x0 cr2page=");
    serial_hex(thread_current()->stack_lo - THREAD_GUARD_SIZE);
    serial_puts(" stack=ist1\r\n");
    recurse(0);
}
#endif

void selftest_sched_case(const struct boot_info *bi, int self_tests_failed)
{
    (void)bi;
#ifdef SCHED_CASE
    if (self_tests_failed) {
        serial_puts("sched-test: skipped, a self-test failed\r\n");
        return;
    }
#endif
    (void)self_tests_failed;
#ifdef VOS_TEST_CASE_thread_overflow
    thread_create("overflow", overflow_worker, 0);
    thread_yield();
#endif
#ifdef VOS_TEST_CASE_sched_deadlock
    static struct wait_queue never;
    serial_puts("sched-test: the boot thread blocks on a queue nobody wakes\r\n");
    thread_block(&never);
#endif
#ifdef SCHED_CASE
    serial_puts("sched-test: the case returned\r\n");
    panic("test case: expected failure did not happen");
#endif
}
