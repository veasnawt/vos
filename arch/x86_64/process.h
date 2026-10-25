/*
 * User processes (N1.5; concurrent and preemptive since N2.3).
 *
 * Every process has one thread of its own (thread.h), created by proc_start
 * and owned by the process. The thread runs on its own guarded kernel stack,
 * which is TSS.RSP0 while it runs, in the process's address space (CR3 =
 * root, switched by the scheduler). It enters ring 3 once (user_enter);
 * every later kernel entry is an interrupt on top of that stack. A system
 * call or an interrupt returns to the same process after frame validation;
 * a timer interrupt may first switch to another thread (preemption, only on
 * this return path to ring 3, after the IRQ was acknowledged). Exit, a user
 * exception or a kill ends the thread; the process keeps its final state.
 * proc_wait blocks until the thread is gone, and only then may proc_destroy
 * free the address space (never the active one). Up to PROC_TABLE processes
 * exist at once; process IDs are never reused.
 *
 * Address space: a fresh PML4 whose only upper-half entry is PML4[511],
 * copied by value from the kernel PML4 (supervisor-only alias; its tables
 * belong to the kernel image and are never freed here). The lower half holds
 * only pages added with proc_add_page (the image, inside the user image
 * region) and the stack, all from freshly allocated, zeroed frames:
 *   image pages                      user, with the permissions recorded per page
 *   USER_STACK_START..USER_STACK_END four pages, user, read + write, NX
 * and nothing else: the null guard, the stack guard page below the stack and
 * the top lower-half page stay unmapped. Every page is checked against its
 * recorded permissions before the process may run.
 *
 * Ownership: the process owns its root frame, the data frames listed in
 * pages[], and the lower-half page tables vmm_map created for them (released
 * by vmm_unmap as they become empty). proc_destroy releases exactly these,
 * only while the kernel root is active, and panics on any inconsistency.
 */
#ifndef VOS_X86_64_PROCESS_H
#define VOS_X86_64_PROCESS_H

#include <stddef.h>
#include <stdint.h>

#include "memlayout.h"
#include "channel.h"
#include "thread.h"

#define PROC_TABLE          8       /* processes that may exist at once */

#define PROC_CODE_VA        USER_IMAGE_START
#define PROC_STACK_PAGES    4
#define PROC_ENTRY_RSP      (USER_STACK_END - 16)   /* 16-byte aligned; 16 zero bytes above */
#define PROC_IMAGE_PAGES    256                     /* 1 MiB, the ELF image limit */
#define PROC_MAX_PAGES      (PROC_IMAGE_PAGES + PROC_STACK_PAGES)

/* States. */
#define PROC_FREE       0
#define PROC_READY      1       /* address space built and audited */
#define PROC_RUNNING    2       /* its thread exists (running, runnable or preempted) */
#define PROC_EXITED     3       /* exit system call */
#define PROC_KILLED     4       /* user exception or unsupported request */

/* Results of proc_create / proc_run (negative, so they never equal a state). */
#define PROC_OK         0
#define PROC_EINVAL     (-1)
#define PROC_EBUSY      (-2)    /* the process table is full */
#define PROC_ENOMEM     (-3)
#define PROC_ESTATE     (-4)

/* The CPU-pushed frame (and syscall registers) of the event that ended the process. */
struct proc_trap {
    uint64_t vector, error, rip, cs, rflags, rsp, ss, cr2;
    uint64_t rax, rdi;
    uint64_t kframe;                    /* kernel address of the saved frame (top of its kernel stack) */
};

#define PROC_NAME_MAX   15

struct process {
    int state;
    uint64_t pid;                       /* never reused */
    char name[PROC_NAME_MAX + 1];       /* kernel-assigned; [a-z0-9_-] only */
    uint64_t root;                      /* PML4 frame */
    uint64_t entry, user_rsp;
    uint32_t abi;                       /* VOS ABI version of its executable (0 or 1) */
    uint64_t tid;                       /* its thread (0 before proc_start) */
    uint64_t kstack_top;                /* that thread's kernel stack top = TSS.RSP0 while it runs */
    struct wait_queue waiters;          /* threads in proc_wait */
    const char *kill_reason;            /* proc_kill: end at the next kernel entry */
    struct handle handles[HANDLE_MAX];  /* N2.4: channel endpoints it owns (channel.h) */
    /* Kernel record of its closed endpoints: messages that arrived for it and that it took out. */
    uint64_t ipc_in, ipc_out, ipc_in_hash, ipc_out_hash;
    int exit_status;
    const char *reason;                 /* why it was killed */
    struct proc_trap trap;
    uint64_t syscalls;                  /* system calls that returned to the process */
    /* How those calls ended, counted by the kernel (a trusted record for tests). */
    uint64_t sys_ok, sys_enosys, sys_einval, sys_efault;
    /* Hardware interrupts taken while it ran in ring 3 (N2.1), and the last one's frame. */
    uint64_t irqs, ticks, tick_mark, tick_budget;
    struct proc_trap last_irq;
    uint64_t preemptions;               /* timer interrupts that switched to another thread */
    uint64_t traps;                     /* kernel entries from ring 3, each validated */
    unsigned npages;
    uint64_t page_va[PROC_MAX_PAGES], page_pa[PROC_MAX_PAGES];
    uint8_t page_flags[PROC_MAX_PAGES]; /* VMM_* the page was mapped with */
};

struct exc_frame;

/*
 * Building an address space in steps (used by the ELF loader):
 *   proc_begin     root with the kernel half; the process now exists
 *   proc_add_page  map a fresh zeroed frame at va (user image region only)
 *                  with VMM_USER and the given W/X; returns the frame so the
 *                  caller can fill it through the kernel alias
 *   proc_finish    map the stack, set the entry point, verify every page,
 *                  mark READY
 * On any error the caller must proc_destroy (which releases everything).
 */
int proc_begin(struct process *p, const char *name);
int proc_add_page(struct process *p, uint64_t va, unsigned flags, uint64_t *pa);
int proc_finish(struct process *p, uint64_t entry);

/*
 * Build an address space holding `len` bytes of code (1..4096). `name` is
 * chosen by the kernel; characters outside [a-z0-9_-] become '_' and it is
 * cut to PROC_NAME_MAX characters.
 */
int proc_create(struct process *p, const char *name, const void *code, size_t len);
/*
 * Start a READY process: create its thread (PROC_ENOMEM if the thread pool
 * is full; the process stays READY). A process starts at most once.
 */
int proc_start(struct process *p);
/* Block until a started process has ended and its thread is gone; returns PROC_EXITED or PROC_KILLED. */
int proc_wait(struct process *p);
/*
 * The N1 interface: proc_start, then proc_wait. Returns the final state
 * (PROC_EXITED or PROC_KILLED), or PROC_ESTATE if it is not READY, or
 * PROC_ENOMEM.
 */
int proc_run(struct process *p);
/*
 * End a started process with the given reason at its next kernel entry (at
 * the latest the next timer interrupt), or before it first enters ring 3.
 */
void proc_kill(struct process *p, const char *reason);
/*
 * N2.5: check the process table: no duplicates, every running process has
 * its thread on its own kernel stack, ended processes hold no thread and no
 * handles, every handle names an endpoint the process owns.
 */
const char *proc_audit(void);
/* Processes that exist (begun and not destroyed). */
unsigned proc_count(void);
/* Kernel file layer: registered READY/RUNNING process, before dereference. */
int proc_file_live(const struct process *p);
/*
 * Called by the exception dispatcher for traps whose saved CS has RPL 3, on
 * the current process's thread.
 * A hardware interrupt is handled and acknowledged (irq_handle), counted,
 * the frame validated, and TRAP_RESUME returned; the program continues
 * exactly where it was interrupted. A non-terminating system call is
 * dispatched, its result stored in the frame's rax, the frame validated, and
 * TRAP_RESUME returned (the stub IRETQs to the same process). Exit and every exception record the
 * CPU-pushed frame, end the process, switch to the kernel CR3 and resume
 * proc_run's caller instead; the process never runs again.
 */
uint64_t proc_user_trap(struct exc_frame *f);
/*
 * Release everything the process owns. Allowed before it started, or after
 * proc_wait; never while its thread exists or its address space is active.
 */
void proc_destroy(struct process *p);

#if defined(VOS_EXEC_VERIFY) && !defined(VOS_TEST)
void proc_test_frame_budget(int n);
#endif

#ifdef VOS_TEST
/* Test hook: allow n more data-frame allocations in proc_create (-1 = unlimited). */
void proc_test_frame_budget(int n);
/*
 * Test hook: kill every later process after n timer ticks in ring 3 (0 =
 * none). Test builds start with PROC_TEST_TICK_BUDGET, so a program that
 * never ends cannot hang a test run; it is killed with reason
 * PROC_REASON_BUDGET. Production builds have no budget (and no scheduler).
 */
#define PROC_TEST_TICK_BUDGET   1000
void proc_test_tick_budget(uint64_t n);
extern const char PROC_REASON_BUDGET[], PROC_REASON_IRQ_BUDGET[];
/* Test hook: why resume would refuse frame f of process p (NULL if it would accept it). */
const char *proc_test_frame_problem(const struct process *p, const struct exc_frame *f);
#endif

#endif
