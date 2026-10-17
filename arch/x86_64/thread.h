/*
 * Kernel threads (N2.2): a fixed pool of thread records and guarded kernel
 * stacks, a cooperative round-robin scheduler and an idle thread.
 *
 * Pool. THREAD_MAX records (16 since N2.3: the stacks still fit the 2 MiB
 * covered by the kernel map's 4 KiB table, which a linker assertion checks);
 * record 0 is the boot thread (kernel_main, on the
 * boot stack from boot.S), the others get one of THREAD_STACKS static 16 KiB
 * stacks in .bss. Each stack sits directly above its own 4 KiB guard page,
 * which the W^X kernel map leaves unmapped (kmap_guard_pages), and starts
 * with canary words. A slot is reused only after the thread that owned it is
 * dead and has been reaped, which happens after the switch away from it.
 * Thread IDs are 64-bit, start at 1 and are never reused.
 *
 * Context switch. A function call (thread_switch.S) that saves rbx, rbp,
 * r12-r15 and RSP: everything the System V ABI requires a callee to keep.
 * All threads share the kernel address space (CR3 never changes) and the
 * kernel has no FPU/SIMD state (-mgeneral-regs-only, CR0.EM).
 *
 * Interrupt state. Every scheduler entry point runs with IF = 0, from thread
 * context, never from an interrupt handler, while no user process runs. The
 * kernel stays non-preemptive: a thread runs until it yields, blocks, sleeps
 * or exits. The only kernel code that runs with IF = 1 is the idle thread's
 * "sti; hlt" (and the cli right after it); a hardware interrupt taken there
 * is acknowledged and returns to the idle loop (sched_idle_irq). Tick
 * sleepers are woken by the idle thread in thread context, not by the IRQ.
 *
 * User programs. proc_run stays synchronous and is allowed only on the boot
 * thread; TSS.RSP0 stays the single dedicated entry stack and never points
 * to a thread stack. Interrupts from ring 3 never switch threads.
 */
#ifndef VOS_X86_64_THREAD_H
#define VOS_X86_64_THREAD_H

#include <stdint.h>

#define THREAD_MAX          16
#define THREAD_STACKS       (THREAD_MAX - 1)    /* the boot thread keeps the boot stack */
#define THREAD_STACK_SIZE   16384
#define THREAD_GUARD_SIZE   4096
#define THREAD_NAME_MAX     15

/* States. */
#define THREAD_FREE         0   /* record unused */
#define THREAD_RUNNABLE     1   /* on the run queue (or the idle thread, waiting its turn) */
#define THREAD_RUNNING      2
#define THREAD_BLOCKED      3   /* on a wait queue, or sleeping until a tick */
#define THREAD_DEAD         4   /* exited; reaped after the switch away */

/* Results. */
#define THREAD_OK           0
#define THREAD_ENOMEM       (-1)    /* no free record or stack */
#define THREAD_EINVAL       (-2)

typedef void (*thread_fn)(void *arg);

struct thread;

/* A FIFO of blocked threads; the wakeup mechanism later IPC will use. */
struct wait_queue {
    struct thread *head, *tail;
};

struct thread {
    uint64_t id;
    int state;
    char name[THREAD_NAME_MAX + 1];
    uint64_t rsp;                       /* saved by context_switch while not running */
    int stack;                          /* stack slot, -1 for the boot stack */
    uint64_t stack_lo, stack_hi;        /* [lo, hi): the usable stack */
    thread_fn entry;
    void *arg;
    struct thread *next;                /* run queue or wait queue link */
    struct wait_queue *waiting_on;
    uint64_t wake_tick;                 /* sleeping: wake at this tick (0 = not sleeping) */
    uint64_t switches_in;               /* times this thread was switched to */
};

struct sched_stats {
    uint64_t switches, yields, blocks, wakeups, sleeps, exits, reaped;
    uint64_t idle_halts, idle_irqs;     /* hlt executions, IRQs taken in ring 0 by the idle thread */
    uint64_t created, create_failed;
};

/* Turn the running boot code into thread 0 and create the idle thread. Once, IF = 0. */
void sched_init(void);
/* Create a runnable kernel thread running entry(arg); returns its ID (> 0) or a THREAD_E* value. */
int64_t thread_create(const char *name, thread_fn entry, void *arg);
struct thread *thread_current(void);
/* Give up the CPU; the caller goes to the end of the run queue. */
void thread_yield(void);
/* Block the caller on q until thread_wake_one/all; the caller must hold no other state. */
void thread_block(struct wait_queue *q);
/* Wake the first (all) thread(s) blocked on q; returns how many were woken. */
int thread_wake_one(struct wait_queue *q);
int thread_wake_all(struct wait_queue *q);
/* Block the caller for at least n timer ticks (woken by the idle thread). */
void thread_sleep(uint64_t ticks);
/* End the calling thread; its stack is reclaimed after the switch away. Not for the boot or idle thread. */
__attribute__((noreturn)) void thread_exit(void);

/* State of the thread with this ID: THREAD_FREE once it is gone (IDs are never reused). */
int thread_state(uint64_t id);
int thread_is_boot(void);
const struct sched_stats *sched_stats(void);
/* Virtual addresses of the guard pages below the thread stacks; returns their number. */
unsigned thread_guard_pages(uint64_t *va, unsigned max);
/* Nonzero if every live thread's stack canaries are intact. */
int thread_canaries_intact(void);
/* Number of records not FREE (live, plus dead ones not yet reaped). */
unsigned thread_count(void);

struct exc_frame;
/*
 * Called for a hardware-interrupt vector taken in ring 0: returns 1 if the
 * frame is the idle thread halted at its "sti; hlt" (the only legal case)
 * after counting it, else 0 (the caller panics).
 */
int sched_idle_irq(const struct exc_frame *f);

#ifdef VOS_TEST
/* Test hooks: the record for an ID (or NULL), and the run-queue order (returns the length). */
struct thread *thread_test_get(uint64_t id);
unsigned sched_test_runq(uint64_t *ids, unsigned max);
/* The ring-0 IRQ check of sched_idle_irq, as if the idle (as_idle) or the current thread ran. */
int sched_test_idle_frame_ok(const struct exc_frame *f, int as_idle);
/* The last ring-0 IRQ frame taken by the idle thread, and the idle thread's stack. */
void sched_test_last_idle_irq(uint64_t *rip, uint64_t *rflags, uint64_t *rsp, uint64_t *idle_lo,
                              uint64_t *idle_hi);
#endif

#endif
