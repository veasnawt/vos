/*
 * Minimal process: one synchronous user program at a time (N1.5).
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

#define PROC_CODE_VA        USER_IMAGE_START
#define PROC_STACK_PAGES    4
#define PROC_ENTRY_RSP      (USER_STACK_END - 16)   /* 16-byte aligned; 16 zero bytes above */
#define PROC_IMAGE_PAGES    256                     /* 1 MiB, the ELF image limit */
#define PROC_MAX_PAGES      (PROC_IMAGE_PAGES + PROC_STACK_PAGES)

/* States. */
#define PROC_FREE       0
#define PROC_READY      1       /* address space built and audited */
#define PROC_RUNNING    2
#define PROC_EXITED     3       /* exit system call */
#define PROC_KILLED     4       /* user exception or unsupported request */

/* Results of proc_create / proc_run (negative, so they never equal a state). */
#define PROC_OK         0
#define PROC_EINVAL     (-1)
#define PROC_EBUSY      (-2)    /* another process exists */
#define PROC_ENOMEM     (-3)
#define PROC_ESTATE     (-4)

/* The CPU-pushed frame (and syscall registers) of the event that ended the process. */
struct proc_trap {
    uint64_t vector, error, rip, cs, rflags, rsp, ss, cr2;
    uint64_t rax, rdi;
    uint64_t kframe;                    /* kernel address of the saved frame (on the RSP0 stack) */
};

#define PROC_NAME_MAX   15

struct process {
    int state;
    char name[PROC_NAME_MAX + 1];       /* kernel-assigned; [a-z0-9_-] only */
    uint64_t root;                      /* PML4 frame */
    uint64_t entry, user_rsp;
    uint64_t kernel_rsp;                /* saved by enter_user, consumed by user_return */
    int exit_status;
    const char *reason;                 /* why it was killed */
    struct proc_trap trap;
    uint64_t syscalls;                  /* system calls that returned to the process */
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
 * Run a READY process in ring 3 until it exits or is killed; returns the
 * final state (PROC_EXITED or PROC_KILLED), or PROC_ESTATE if it is not
 * READY. A process runs at most once.
 */
int proc_run(struct process *p);
/*
 * Called by the exception dispatcher for traps whose saved CS has RPL 3.
 * A non-terminating system call is dispatched, its result stored in the
 * frame's rax, the frame validated, and TRAP_RESUME returned (the stub
 * IRETQs to the same process). Exit and every exception record the
 * CPU-pushed frame, end the process, switch to the kernel CR3 and resume
 * proc_run's caller instead; the process never runs again.
 */
uint64_t proc_user_trap(struct exc_frame *f);
/* Release everything the process owns; the kernel root must be active. */
void proc_destroy(struct process *p);

#ifdef VOS_TEST
/* Test hook: allow n more data-frame allocations in proc_create (-1 = unlimited). */
void proc_test_frame_budget(int n);
#endif

#endif
