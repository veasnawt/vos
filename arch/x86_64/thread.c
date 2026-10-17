#include "thread.h"
#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "lib.h"
#include "memlayout.h"
#include "panic.h"

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
    (void)idle;
    (void)stack_take;
    (void)stack_release;

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
