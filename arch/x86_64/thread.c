#include "thread.h"
#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "lib.h"
#include "memlayout.h"
#include "panic.h"
#include "process.h"
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

/* Every switch happens with IF = 0, on the kernel address space, with no user program running. */
static void check_switch(const struct thread *next)
{
    if (read_rflags() & RFLAGS_IF)
        panic("sched: switch with interrupts enabled");
    if ((read_cr3() & PTE_ADDR) != vmm_kernel_root())
        panic("sched: switch outside the kernel address space");
    if (proc_running())
        panic("sched: switch while a user program runs");
    if (next->state != THREAD_RUNNABLE)
        panic("sched: switching to a thread that is not runnable");
    if (next->rsp < next->stack_lo || next->rsp >= next->stack_hi || next->rsp % 16 != 8)
        panic("sched: saved stack pointer outside the thread's own stack");
    if (next->stack >= 0 && stack_owner[next->stack] != next)
        panic("sched: thread runs on a stack it does not own");
    if (!canaries_ok(current) || !canaries_ok(next))
        panic("sched: thread stack canary overwritten");
    if (gdt_tss_rsp0() > next->stack_lo && gdt_tss_rsp0() <= next->stack_hi)
        panic("sched: TSS.RSP0 points into a thread stack");
}

static void reap(struct thread *t)
{
    if (t->state != THREAD_DEAD || t == current || t == idle || t == &pool[0])
        panic("sched: reaping a thread that is not dead");
    stack_release(t);
    memset(t, 0, sizeof(*t));           /* THREAD_FREE; the ID is never reused */
    stats.reaped++;
}

/* Runs in the thread that was just switched to: release the previous one if it died. */
static void finish_switch(void)
{
    struct thread *prev = switched_from;

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
    current = next;
    switched_from = prev;
    stats.switches++;
    context_switch(&prev->rsp, next->rsp);
    finish_switch();                    /* back in prev, after some other thread switched to it */
}

/* Pick the next thread: the run-queue head, else the idle thread (if any), else keep running. */
static void schedule(void)
{
    struct thread *next = runq_pop();

    if (!next && idle && current != idle)
        next = idle;
    if (!next || next == current) {
        if (current->state != THREAD_RUNNABLE && current->state != THREAD_RUNNING)
            panic("sched: nothing to run");
        current->state = THREAD_RUNNING;
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

int64_t thread_create(const char *name, thread_fn entry, void *arg)
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
    t->state = THREAD_RUNNABLE;
    runq_push(t);
    stats.created++;
    return (int64_t)t->id;
}

static void check_entry(void)
{
    if (!current)
        panic("sched: not initialized");
    if (read_rflags() & RFLAGS_IF)
        panic("sched: called with interrupts enabled");
}

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
    serial_puts("\r\n");
}
