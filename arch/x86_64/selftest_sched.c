/*
 * Kernel threads and scheduler (N2.2), VOS_TEST only. Every verdict is the
 * kernel's own: the threads under test are trusted kernel code that record
 * what they observed in kernel memory, and the boot thread checks it.
 */
#include "selftest.h"
#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "kmap.h"
#include "lib.h"
#include "memlayout.h"
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

int selftest_sched(void)
{
    failures = cases = 0;
    test_stacks();
    test_order();
    test_registers();
    test_isolation();
    test_exit_and_reap();
    test_exhaustion();
    expect(thread_is_boot() && thread_canaries_intact() && !(read_rflags() & RFLAGS_IF),
           "back on the boot thread with IF=0 and every canary intact");
    serial_puts("selftest: sched ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}
