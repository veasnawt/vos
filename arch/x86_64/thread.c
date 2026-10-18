#include "thread.h"
#include "console.h"
#include "cpu.h"
#include "exception.h"
#include "gdt.h"
#include "irq.h"
#include "lib.h"
#include "memlayout.h"
#include "panic.h"
#include "vmm.h"

#define STACK_CANARY        0x7EADC0DE7EADC0DEULL
#define CANARY_WORDS        8       /* at the lowest addresses: an overflow hits them first */
#ifdef VOS_TEST
#define STACK_POISON        0xDD    /* a reaped stack is filled with this in test builds */
#endif

extern char kernel_stack[], kernel_stack_top[];

/* Each stack directly above its own guard page (unmapped by kmap.c). */
struct thread_stack {
    uint8_t guard[THREAD_GUARD_SIZE];
    uint8_t stack[THREAD_STACK_SIZE];
};
static struct thread_stack stacks[THREAD_STACKS] __attribute__((aligned(4096)));
static struct thread *stack_owner[THREAD_STACKS];

static struct thread pool[THREAD_MAX];
static uint64_t next_id = 1;
static struct thread *current, *idle;
static struct sched_stats stats;
static int device_woke;                 /* N3.4: a bottom half made a thread runnable since the last switch */

unsigned thread_guard_pages(uint64_t *va, unsigned max)
{
    unsigned n = 0;

    for (unsigned i = 0; i < THREAD_STACKS && n < max; i++)
        va[n++] = (uint64_t)stacks[i].guard;
    return n;
}

static void set_canaries(int slot)
{
    uint64_t *c = (uint64_t *)stacks[slot].stack;

    for (int i = 0; i < CANARY_WORDS; i++)
        c[i] = STACK_CANARY;
}

static int canaries_ok(const struct thread *t)
{
    if (t->stack < 0)
        return 1;                       /* the boot stack has its guard page only */
    const uint64_t *c = (const uint64_t *)stacks[t->stack].stack;

    for (int i = 0; i < CANARY_WORDS; i++)
        if (c[i] != STACK_CANARY)
            return 0;
    return 1;
}

int thread_canaries_intact(void)
{
    for (unsigned i = 0; i < THREAD_MAX; i++)
        if (pool[i].state != THREAD_FREE && !canaries_ok(&pool[i]))
            return 0;
    return 1;
}

/* A free stack slot for t, or -1. A slot is free only after its last owner was reaped. */
static int stack_take(struct thread *t)
{
    for (int i = 0; i < THREAD_STACKS; i++) {
        if (stack_owner[i])
            continue;
        stack_owner[i] = t;
        set_canaries(i);
        t->stack = i;
        t->stack_lo = (uint64_t)stacks[i].stack;
        t->stack_hi = t->stack_lo + THREAD_STACK_SIZE;
        return i;
    }
    return -1;
}

static void stack_release(struct thread *t)
{
    int i = t->stack;

    if (i < 0 || i >= THREAD_STACKS || stack_owner[i] != t)
        panic("thread: releasing a stack the thread does not own");
    if (!canaries_ok(t))
        panic("thread: stack canary overwritten");
    if (t == current || (gdt_tss_rsp0() > t->stack_lo && gdt_tss_rsp0() <= t->stack_hi))
        panic("thread: releasing a stack still in use");
#ifdef VOS_TEST
    memset(stacks[i].stack, STACK_POISON, THREAD_STACK_SIZE);
#endif
    stack_owner[i] = NULL;
    t->stack = -1;
    t->stack_lo = t->stack_hi = 0;
}

static struct thread *record_take(const char *name)
{
    for (unsigned i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &pool[i];

        if (t->state != THREAD_FREE)
            continue;
        memset(t, 0, sizeof(*t));
        t->id = next_id++;
        t->stack = -1;
        for (int k = 0; k < THREAD_NAME_MAX && name && name[k]; k++)
            t->name[k] = name[k];
        return t;
    }
    return NULL;
}

struct thread *thread_current(void)
{
    return current;
}

int thread_is_boot(void)
{
    return current == &pool[0];
}

unsigned thread_count(void)
{
    unsigned n = 0;

    for (unsigned i = 0; i < THREAD_MAX; i++)
        n += pool[i].state != THREAD_FREE;
    return n;
}

int thread_state(uint64_t id)
{
    for (unsigned i = 0; i < THREAD_MAX; i++)
        if (pool[i].state != THREAD_FREE && pool[i].id == id)
            return pool[i].state;
    return THREAD_FREE;
}

uint64_t thread_stack_top(uint64_t id)
{
    for (unsigned i = 0; i < THREAD_MAX; i++)
        if (pool[i].state != THREAD_FREE && pool[i].id == id)
            return pool[i].stack_hi;
    return 0;
}

const struct sched_stats *sched_stats(void)
{
    return &stats;
}

#ifdef VOS_TEST
struct thread *thread_test_get(uint64_t id)
{
    for (unsigned i = 0; i < THREAD_MAX; i++)
        if (pool[i].state != THREAD_FREE && pool[i].id == id)
            return &pool[i];
    return NULL;
}
#endif

/* ---- run queue ---- */

static struct thread *runq_head, *runq_tail;
static unsigned runq_len;

static void runq_push(struct thread *t)
{
    if (t == idle || t->state != THREAD_RUNNABLE || t->next || t == runq_tail)
        panic("sched: bad run-queue insertion");
    if (runq_tail)
        runq_tail->next = t;
    else
        runq_head = t;
    runq_tail = t;
    runq_len++;
}

static struct thread *runq_pop(void)
{
    struct thread *t = runq_head;

    if (!t)
        return NULL;
    runq_head = t->next;
    if (!runq_head)
        runq_tail = NULL;
    t->next = NULL;
    runq_len--;
    if (t->state != THREAD_RUNNABLE || t == idle)
        panic("sched: run queue holds a thread that is not runnable");
    return t;
}

#ifdef VOS_TEST
unsigned sched_test_runq(uint64_t *ids, unsigned max)
{
    unsigned n = 0;

    for (struct thread *t = runq_head; t && n < max; t = t->next)
        ids[n++] = t->id;
    return n;
}
#endif

/* ---- switching ---- */

void context_switch(uint64_t *save_rsp, uint64_t load_rsp);   /* thread_switch.S */
extern char thread_start[];

static struct thread *switched_from;

/* The kernel half (PML4[511]) of root is the kernel's, supervisor-only. */
static int shares_kernel_half(uint64_t root)
{
    const uint64_t *l4 = phys_to_virt(root), *kl4 = phys_to_virt(vmm_kernel_root());

    return root && root % PAGE_SIZE == 0 && root < BOOT_MAP_LIMIT && l4[511] == kl4[511] &&
           !(l4[511] & PTE_U);
}

/* The TSS.RSP0 a thread needs while it runs: its own stack top if it runs a user process. */
static uint64_t rsp0_for(const struct thread *t)
{
    return t->proc ? t->stack_hi : gdt_entry_stack_top();
}

/*
 * Every switch happens with IF = 0, in thread context, from a thread running
 * on its own address space, to a runnable thread whose address space shares
 * the kernel half: a kernel thread on the kernel root, a user thread on its
 * process's root (never the kernel root).
 */
static void check_switch(const struct thread *next)
{
    if (read_rflags() & RFLAGS_IF)
        panic("sched: switch with interrupts enabled");
    if ((read_cr3() & PTE_ADDR) != current->root)
        panic("sched: the running thread is not on its own address space");
    if (!shares_kernel_half(next->root) || (next->proc ? next->root == vmm_kernel_root()
                                                       : next->root != vmm_kernel_root()))
        panic("sched: target address space is not valid for the thread");
    if (next->state != THREAD_RUNNABLE)
        panic("sched: switching to a thread that is not runnable");
    if (next->rsp < next->stack_lo || next->rsp >= next->stack_hi || next->rsp % 16 != 8)
        panic("sched: saved stack pointer outside the thread's own stack");
    if (next->stack >= 0 && stack_owner[next->stack] != next)
        panic("sched: thread runs on a stack it does not own");
    if (!canaries_ok(current) || !canaries_ok(next))
        panic("sched: thread stack canary overwritten");
    if (next->stack < 0 && next->proc)
        panic("sched: a user thread without its own kernel stack");
    /* Every interrupt is acknowledged before any switch (preemption included). */
    if (irq_chip() && irq_chip()->in_service())
        panic("sched: switch while an interrupt is still in service");
}

/* Runs in the thread just switched to: its address space and RSP0 must be its own. */
static void check_arrival(void)
{
    if ((read_cr3() & PTE_ADDR) != current->root)
        panic("sched: switched to a thread on a foreign address space");
    if (gdt_tss_rsp0() != rsp0_for(current))
        panic("sched: TSS.RSP0 is not the running thread's");
}

static void reap(struct thread *t)
{
    if (t->state != THREAD_DEAD || t == current || t == idle || t == &pool[0])
        panic("sched: reaping a thread that is not dead");
    if (t->waiting_on || t->next)
        panic("sched: a dead thread is still linked on a queue");
    stack_release(t);
    memset(t, 0, sizeof(*t));           /* THREAD_FREE; the ID is never reused */
    stats.reaped++;
}

/* Runs in the thread that was just switched to: release the previous one if it died. */
static void finish_switch(void)
{
    struct thread *prev = switched_from;

    check_arrival();
    switched_from = NULL;
    if (prev && prev->state == THREAD_DEAD)
        reap(prev);
}

static void switch_to(struct thread *next)
{
    struct thread *prev = current;

    check_switch(next);
    next->state = THREAD_RUNNING;
    next->switches_in++;
    next->slice_start = irq_ticks();
    device_woke = 0;
    current = next;
    switched_from = prev;
    stats.switches++;
    /* The next thread's kernel-entry stack and address space (the kernel half is shared). */
    gdt_set_rsp0(rsp0_for(next));
    if ((read_cr3() & PTE_ADDR) != next->root)
        write_cr3(next->root);          /* flushes every non-global translation */
    context_switch(&prev->rsp, next->rsp);
    finish_switch();                    /* back in prev, after some other thread switched to it */
}

static void unlink_waiter(struct wait_queue *q, struct thread *t);

/*
 * Make every sleeper whose tick has come runnable, and end every queue wait
 * whose deadline has come (N3.4: the thread leaves its queue first). Thread
 * context, IF = 0; never in an IRQ.
 */
static void wake_sleepers(void)
{
    uint64_t now = irq_ticks();

    for (unsigned i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &pool[i];

        if (t->state == THREAD_BLOCKED && t->wake_tick && now >= t->wake_tick) {
            if (now - t->wake_tick > stats.max_wake_late)
                stats.max_wake_late = now - t->wake_tick;
            if (t->waiting_on) {
                unlink_waiter(t->waiting_on, t);
                t->waiting_on = NULL;
                stats.timeouts++;
            }
            t->wake_tick = 0;
            t->wake_reason = THREAD_TIMEDOUT;
            t->state = THREAD_RUNNABLE;
            runq_push(t);
            stats.wakeups++;
        }
    }
}

/* Pick the next thread: the run-queue head, else the idle thread (if any), else keep running. */
static void schedule(void)
{
    wake_sleepers();
    struct thread *next = runq_pop();

    if (!next && idle && current != idle)
        next = idle;
    if (!next || next == current) {
        if (current->state != THREAD_RUNNABLE && current->state != THREAD_RUNNING)
            panic("sched: nothing to run");
        current->state = THREAD_RUNNING;
        /*
         * N2.5 livelock detector: the kernel is not preemptible and runs with
         * IF = 0, so a thread that keeps yielding to itself without time
         * passing (no tick can arrive) would hang the machine silently.
         */
        static uint64_t idle_yields, at_tick;
        if (irq_ticks() != at_tick) {
            at_tick = irq_ticks();
            idle_yields = 0;
        } else if (++idle_yields > 1000000) {
            panic("sched: a thread spins in the kernel with nothing else runnable (busy wait?)");
        }
        return;
    }
    if (next == idle)
        idle->state = THREAD_RUNNABLE;
    switch_to(next);
}

/* First code of a new thread (from thread_start). */
__attribute__((noreturn)) void thread_first_run(void)
{
    finish_switch();
    if (read_rflags() & RFLAGS_IF)
        panic("sched: new thread started with interrupts enabled");
    current->entry(current->arg);
    thread_exit();
}

static int64_t create(const char *name, thread_fn entry, void *arg, int enqueue, uint64_t root, void *proc)
{
    struct thread *t;

    if (!current || !entry)
        return THREAD_EINVAL;
    if (!(t = record_take(name))) {
        stats.create_failed++;
        return THREAD_ENOMEM;
    }
    if (stack_take(t) < 0) {
        memset(t, 0, sizeof(*t));
        stats.create_failed++;
        return THREAD_ENOMEM;
    }
    /*
     * Initial frame, as context_switch leaves it: six callee-saved registers
     * (zero), then the return address thread_start. After its ret, RSP is
     * 16-byte aligned for thread_start's call; the two words above are zero.
     */
    uint64_t *sp = (uint64_t *)t->stack_hi;
    *--sp = 0;
    *--sp = 0;
    *--sp = (uint64_t)thread_start;
    for (int i = 0; i < 6; i++)
        *--sp = 0;
    t->rsp = (uint64_t)sp;
    t->entry = entry;
    t->arg = arg;
    t->root = root;
    t->proc = proc;
    t->state = THREAD_RUNNABLE;
    if (enqueue)
        runq_push(t);
    else
        idle = t;                       /* the idle thread is never on the run queue */
    stats.created++;
    return (int64_t)t->id;
}

int64_t thread_create(const char *name, thread_fn entry, void *arg)
{
    return create(name, entry, arg, 1, vmm_kernel_root(), NULL);
}

int64_t thread_create_user(const char *name, thread_fn entry, void *arg, uint64_t root, void *proc)
{
    if (!proc || root == vmm_kernel_root() || !shares_kernel_half(root))
        return THREAD_EINVAL;
    return create(name, entry, arg, 1, root, proc);
}

void thread_leave_user_space(void)
{
    if (!current->proc)
        panic("sched: a kernel thread has no user space to leave");
    current->proc = NULL;
    current->root = vmm_kernel_root();
    write_cr3(current->root);
    gdt_set_rsp0(rsp0_for(current));
}

int sched_preempt_due(void)
{
    return current && current->proc && runq_head &&
           (device_woke || irq_ticks() - current->slice_start >= SCHED_SLICE_TICKS);
}

static void check_entry(void)
{
    if (!current)
        panic("sched: not initialized");
    if (read_rflags() & RFLAGS_IF)
        panic("sched: called with interrupts enabled");
    if (irq_in_handler())
        panic("sched: called from an interrupt handler");
}

void sched_tick(void)
{
    uint64_t w0 = stats.wakeups;

    check_entry();
    irq_run_deferred();                 /* device bottom halves (N3.4) */
    if (stats.wakeups != w0) {
        device_woke = 1;                /* a waiter of a device need not wait for the slice to end */
        stats.device_wakeups += stats.wakeups - w0;
    }
    wake_sleepers();
}

int thread_block_until(struct wait_queue *q, uint64_t deadline)
{
    check_entry();
    if (current == idle || !q)
        panic("sched: the idle thread never blocks");
    if (deadline && irq_ticks() >= deadline)
        return THREAD_TIMEDOUT;         /* already due: never enqueued */
    current->state = THREAD_BLOCKED;
    current->waiting_on = q;
    current->wake_tick = deadline;
    current->wake_reason = THREAD_WOKEN;
    current->next = NULL;
    if (q->tail)
        q->tail->next = current;
    else
        q->head = current;
    q->tail = current;
    stats.blocks++;
    schedule();
    return current->wake_reason;
}

void thread_block(struct wait_queue *q)
{
    thread_block_until(q, 0);
}

int thread_wake_one(struct wait_queue *q)
{
    struct thread *t = q->head;

    check_entry();
    if (!t)
        return 0;
    q->head = t->next;
    if (!q->head)
        q->tail = NULL;
    t->next = NULL;
    if (t->state != THREAD_BLOCKED || t->waiting_on != q)
        panic("sched: wait queue holds a thread that is not blocked on it");
    t->waiting_on = NULL;
    t->wake_tick = 0;                   /* its deadline (if any) no longer applies */
    t->wake_reason = THREAD_WOKEN;
    t->state = THREAD_RUNNABLE;
    runq_push(t);
    stats.wakeups++;
    return 1;
}

int thread_wake_all(struct wait_queue *q)
{
    int n = 0;

    while (thread_wake_one(q))
        n++;
    return n;
}

/* Remove t from the wait queue it is blocked on. */
static void unlink_waiter(struct wait_queue *q, struct thread *t)
{
    struct thread *prev = NULL;

    for (struct thread *w = q->head; w; prev = w, w = w->next) {
        if (w != t)
            continue;
        if (prev)
            prev->next = w->next;
        else
            q->head = w->next;
        if (q->tail == w)
            q->tail = prev;
        w->next = NULL;
        return;
    }
    panic("sched: a blocked thread is missing from its wait queue");
}

int thread_interrupt(uint64_t id)
{
    struct thread *t = NULL;

    check_entry();
    for (unsigned i = 0; i < THREAD_MAX && !t; i++)
        if (pool[i].state != THREAD_FREE && pool[i].id == id)
            t = &pool[i];
    if (!t || t->state != THREAD_BLOCKED || t == idle)
        return 0;
    if (t->waiting_on) {
        unlink_waiter(t->waiting_on, t);
        t->waiting_on = NULL;
    }
    t->wake_tick = 0;
    t->wake_reason = THREAD_INTERRUPTED;
    t->state = THREAD_RUNNABLE;
    runq_push(t);
    stats.wakeups++;
    return 1;
}

void thread_sleep(uint64_t ticks)
{
    check_entry();
    if (current == idle)
        panic("sched: the idle thread never sleeps");
    current->wake_tick = irq_ticks() + (ticks ? ticks : 1);
    current->state = THREAD_BLOCKED;
    stats.sleeps++;
    schedule();
}

/* ---- the idle thread ---- */

void idle_halt(void);                   /* thread_switch.S */
extern char idle_after_hlt[];
static uint64_t last_idle_rip, last_idle_rflags, last_idle_rsp;

static int any_sleeper(void)
{
    for (unsigned i = 0; i < THREAD_MAX; i++)
        if (pool[i].state == THREAD_BLOCKED && pool[i].wake_tick)
            return 1;
    return 0;
}

static void idle_main(void *arg)
{
    (void)arg;
    for (;;) {
        irq_run_deferred();             /* device bottom halves (N3.4) */
        wake_sleepers();
        if (runq_head) {
            thread_yield();
            continue;
        }
        /*
         * Nothing can run and no timer will wake anyone. Device waits always
         * have a deadline (N3.4), so this is a lost wakeup even with
         * interrupt-driven devices.
         */
        if (!any_sleeper())
            panic("sched: every thread is blocked and none sleeps (lost wakeup?)");
        stats.idle_halts++;
        idle_halt();                    /* IF = 1 only inside; back with IF = 0 */
        if (read_rflags() & RFLAGS_IF)
            panic("sched: idle resumed with interrupts enabled");
        /* Every IRQ taken in the wait was acknowledged before it returned here. */
        if (irq_chip()->in_service())
            panic("sched: an interrupt is still in service after the idle wait");
    }
}

/* The frame of an IRQ taken by the idle thread in its "sti; hlt", and nothing else. */
static int idle_frame_ok(const struct exc_frame *f, const struct thread *cur)
{
    uint64_t lo = idle ? idle->stack_lo : 0, hi = idle ? idle->stack_hi : 0;

    return idle && cur == idle && f->cs == GDT_KERNEL_CS && f->ss == GDT_KERNEL_DS &&
           f->rip == (uint64_t)idle_after_hlt &&
           (f->rflags & (RFLAGS_IF | RFLAGS_IOPL | (1ULL << 17))) == RFLAGS_IF &&
           f->rsp > lo && f->rsp <= hi && (uint64_t)f > lo && (uint64_t)f < hi;
}

int sched_idle_irq(const struct exc_frame *f)
{
    if (!idle_frame_ok(f, current))
        return 0;
    stats.idle_irqs++;
    last_idle_rip = f->rip;
    last_idle_rflags = f->rflags;
    last_idle_rsp = f->rsp;
    return 1;
}

#ifdef VOS_TEST
int sched_test_idle_frame_ok(const struct exc_frame *f, int as_idle)
{
    return idle_frame_ok(f, as_idle ? idle : current);
}

void sched_test_last_idle_irq(uint64_t *rip, uint64_t *rflags, uint64_t *rsp, uint64_t *idle_lo,
                              uint64_t *idle_hi)
{
    *rip = last_idle_rip;
    *rflags = last_idle_rflags;
    *rsp = last_idle_rsp;
    *idle_lo = idle ? idle->stack_lo : 0;
    *idle_hi = idle ? idle->stack_hi : 0;
}
#endif

void thread_yield(void)
{
    check_entry();
    stats.yields++;
    current->state = THREAD_RUNNABLE;
    if (current != idle)
        runq_push(current);
    schedule();
}

__attribute__((noreturn)) void thread_exit(void)
{
    check_entry();
    if (current == &pool[0] || current == idle)
        panic("sched: the boot and idle threads never exit");
    current->state = THREAD_DEAD;
    stats.exits++;
    schedule();                         /* never returns: the next thread reaps this one */
    panic("sched: a dead thread was resumed");
}

void sched_init(void)
{
    struct thread *boot;

    if (current)
        panic("sched: initialized twice");
    if (read_rflags() & RFLAGS_IF)
        panic("sched: interrupts enabled at init");
    boot = record_take("boot");
    if (boot != &pool[0])
        panic("sched: boot thread record");
    boot->state = THREAD_RUNNING;
    boot->stack_lo = (uint64_t)kernel_stack;
    boot->stack_hi = (uint64_t)kernel_stack_top;
    current = boot;
    boot->root = vmm_kernel_root();
    if (create("idle", idle_main, 0, 0, vmm_kernel_root(), NULL) <= 0 || !idle)
        panic("sched: cannot create the idle thread");

    serial_puts("sched: ");
    serial_dec(THREAD_MAX);
    serial_puts(" thread records, ");
    serial_dec(THREAD_STACKS);
    serial_puts(" guarded stacks of ");
    serial_dec(THREAD_STACK_SIZE / 1024);
    serial_puts(" KiB at ");
    serial_hex((uint64_t)stacks);
    serial_puts("; boot thread id ");
    serial_dec(boot->id);
    serial_puts(", idle thread id ");
    serial_dec(idle->id);
    serial_puts("\r\n");
}

/* ---- consistency audit (N2.5) ---- */

static int on_queue(const struct thread *t, const struct wait_queue *q)
{
    unsigned n = 0;

    for (const struct thread *w = q->head; w && n <= THREAD_MAX; w = w->next, n++)
        if (w == t)
            return 1;
    return 0;
}

const char *sched_audit(void)
{
    unsigned in_runq = 0;

    if (read_rflags() & RFLAGS_IF)
        return "audit called with interrupts enabled";
    for (const struct thread *t = runq_head; t; t = t->next) {
        if (++in_runq > THREAD_MAX)
            return "run queue is cyclic";
        if (t->state != THREAD_RUNNABLE || t == idle || t == current)
            return "run queue holds a thread that is not runnable";
        if (!t->next && t != runq_tail)
            return "run queue tail is wrong";
    }
    if (in_runq != runq_len)
        return "run-queue length differs from its contents";
    for (unsigned i = 0; i < THREAD_MAX; i++) {
        const struct thread *t = &pool[i];
        int queued = 0;

        for (const struct thread *r = runq_head; r; r = r->next)
            queued |= r == t;
        switch (t->state) {
        case THREAD_FREE:
            if (t->next || t->waiting_on || t->proc || t->id)
                return "a free thread record is not clean";
            break;
        case THREAD_RUNNING:
            if (t != current)
                return "a thread other than the current one is running";
            break;
        case THREAD_RUNNABLE:
            if (t != idle && !queued)
                return "a runnable thread is not on the run queue";
            break;
        case THREAD_BLOCKED:
            if (queued)
                return "a blocked thread is on the run queue";
            if (t->waiting_on ? !on_queue(t, t->waiting_on) : !t->wake_tick)
                return "a blocked thread is neither on its wait queue nor sleeping";
            break;
        case THREAD_DEAD:
            return "a dead thread was not reaped";   /* reaped by the next thread that ran */
        default:
            return "a thread record has an invalid state";
        }
        if (t->state != THREAD_BLOCKED && t->wake_tick)
            return "a thread that is not blocked has a wake tick";
        if (t->state != THREAD_FREE && t->stack >= 0 &&
            (stack_owner[t->stack] != t || !canaries_ok(t)))
            return "a thread's stack ownership or canaries are wrong";
        if (t->state != THREAD_FREE && t->proc && t->root == vmm_kernel_root())
            return "a user thread runs on the kernel address space";
    }
    for (unsigned i = 0; i < THREAD_STACKS; i++)
        if (stack_owner[i] && (stack_owner[i]->state == THREAD_FREE || stack_owner[i]->stack != (int)i))
            return "a stack slot is owned by a thread that does not use it";
    return NULL;
}
