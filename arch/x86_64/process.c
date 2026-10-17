#include "process.h"
#include "console.h"
#include "cpu.h"
#include "exception.h"
#include "gdt.h"
#include "idt.h"
#include "irq.h"
#include "syscall.h"
#include "lib.h"
#include "panic.h"
#include "pmm.h"
#include "thread.h"
#include "vmm.h"

/* The one process that may exist (created and not yet destroyed), and the one running. */
static struct process *live, *current;

/* usermode.S */
int enter_user(uint64_t rip, uint64_t rsp, uint64_t cr3, uint64_t *kernel_rsp);
__attribute__((noreturn)) void user_return(uint64_t *kernel_rsp, int result);

#define RFLAGS_FIXED    (1ULL << 1)
#define RFLAGS_VM       (1ULL << 17)

/*
 * Last checks before IRETQ returns to the same process after a system call
 * or a hardware interrupt: user selectors, IF = 1 and IOPL = 0 (ring 3 can
 * change neither), no virtual-8086 flag, lower-half RIP/RSP, its own address
 * space, the frame exactly at the top of the TSS.RSP0 stack (the CPU used
 * the right kernel stack) and intact stack canaries.
 */
static const char *frame_problem(const struct process *p, const struct exc_frame *f)
{
    if (f->cs != GDT_USER_CS || f->ss != GDT_USER_DS)
        return "not user selectors";
    if (!(f->rflags & RFLAGS_IF) || (f->rflags & (RFLAGS_IOPL | RFLAGS_VM)) || !(f->rflags & RFLAGS_FIXED))
        return "RFLAGS not IF=1, IOPL=0, VM=0";
    if (f->rip >= LOWER_HALF_END || f->rsp >= LOWER_HALF_END)
        return "RIP or RSP outside the lower half";
    if ((read_cr3() & PTE_ADDR) != p->root)
        return "not on the process's address space";
    if ((uint64_t)f + sizeof(*f) != gdt_tss_rsp0())
        return "frame not at the top of the TSS.RSP0 stack";
    if (!gdt_stack_canaries_intact())
        return "kernel stack canary overwritten";
    if (read_rflags() & RFLAGS_IF)
        return "kernel running with IF=1";
    return NULL;
}

static uint64_t resume(struct process *p, struct exc_frame *f)
{
    const char *why = frame_problem(p, f);

    if (why) {
        serial_puts("process: invalid user frame: ");
        serial_puts(why);
        serial_puts("\r\n");
        panic("process: refusing to resume an invalid user frame");
    }
    return TRAP_RESUME;
}

static void record(struct proc_trap *t, const struct exc_frame *f)
{
    t->vector = f->vector;
    t->error = f->error;
    t->rip = f->rip;
    t->cs = f->cs;
    t->rflags = f->rflags;
    t->rsp = f->rsp;
    t->ss = f->ss;
    t->cr2 = f->vector == 14 ? read_cr2() : 0;
    t->rax = f->rax;
    t->rdi = f->rdi;
    t->kframe = (uint64_t)f;
}

#ifdef VOS_TEST
static int frame_budget = -1;
static uint64_t tick_budget = PROC_TEST_TICK_BUDGET;
const char PROC_REASON_BUDGET[] = "test tick budget";
const char PROC_REASON_IRQ_BUDGET[] = "test interrupt budget (timer ticks not advancing)";

void proc_test_tick_budget(uint64_t n)
{
    tick_budget = n;
}

const char *proc_test_frame_problem(const struct process *p, const struct exc_frame *f)
{
    return frame_problem(p, f);
}

void proc_test_frame_budget(int n)
{
    frame_budget = n;
}
#endif

static int frame_alloc(uint64_t *pa)
{
#ifdef VOS_TEST
    if (frame_budget == 0) {
        *pa = 0;
        return PMM_ENOMEM;
    }
    if (frame_budget > 0)
        frame_budget--;
#endif
    return pmm_alloc_frame(pa);         /* zeroed */
}

static void must_free(uint64_t pa, const char *what)
{
    if (pmm_free_frame(pa) != PMM_OK)
        panic(what);
}

/* Map a fresh zeroed frame at va and record it. */
static int add_page(struct process *p, uint64_t va, unsigned flags, uint64_t *pa)
{
    *pa = 0;
    if (p->npages >= PROC_MAX_PAGES)
        return PROC_EINVAL;
    if (frame_alloc(pa) != PMM_OK)
        return PROC_ENOMEM;
    int err = vmm_map(p->root, va, *pa, flags);
    if (err != VMM_OK) {
        must_free(*pa, "process: frame release failed");
        *pa = 0;
        return err == VMM_ENOMEM ? PROC_ENOMEM : PROC_EINVAL;
    }
    p->page_va[p->npages] = va;
    p->page_flags[p->npages] = (uint8_t)flags;
    p->page_pa[p->npages++] = *pa;
    return PROC_OK;
}

/* Check the finished address space before anything may run in it. */
static int verify_space(const struct process *p)
{
    const uint64_t *l4 = phys_to_virt(p->root);
    const uint64_t *kl4 = phys_to_virt(vmm_kernel_root());
    struct vmm_audit a;
    struct vmm_mapping m;
    unsigned stack = 0;

    if (l4[511] != kl4[511] || (l4[511] & PTE_U))
        return 0;
    for (int i = 256; i < 511; i++)
        if (l4[i])
            return 0;
    if (vmm_audit_space(p->root, 1, &a) != VMM_OK || a.leaves_lower != p->npages ||
        a.user_leaves != p->npages)
        return 0;
    /* Every page exactly as recorded: frame, user bit and W/X. */
    for (unsigned i = 0; i < p->npages; i++) {
        uint64_t va = p->page_va[i];
        if (vmm_lookup(p->root, va, &m) != VMM_OK || m.pa != p->page_pa[i] ||
            m.page_size != PAGE_SIZE || m.flags != p->page_flags[i] || !(m.flags & VMM_USER))
            return 0;
        if (va >= USER_STACK_START && va < USER_STACK_END) {
            if (m.flags != (VMM_WRITE | VMM_USER))
                return 0;
            stack++;
        } else if (va < USER_IMAGE_START || va >= USER_IMAGE_END) {
            return 0;
        }
    }
    return stack == PROC_STACK_PAGES &&
           vmm_lookup(p->root, 0, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(p->root, NULL_GUARD_END - PAGE_SIZE, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(p->root, USER_STACK_GUARD, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(p->root, USER_STACK_END, &m) == VMM_ENOTMAPPED;
}

int proc_begin(struct process *p, const char *name)
{
    if (live)
        return PROC_EBUSY;
    if (!p)
        return PROC_EINVAL;
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < PROC_NAME_MAX && name && name[i]; i++) {
        char c = name[i];
        p->name[i] = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ? c : '_';
    }
    if (!p->name[0])
        p->name[0] = '_';

    if (frame_alloc(&p->root) != PMM_OK)
        return PROC_ENOMEM;
    /* The kernel half: PML4[511] by value, nothing else. */
    ((uint64_t *)phys_to_virt(p->root))[511] =
        ((const uint64_t *)phys_to_virt(vmm_kernel_root()))[511];
    live = p;
    p->state = PROC_FREE;
    return PROC_OK;
}

int proc_add_page(struct process *p, uint64_t va, unsigned flags, uint64_t *pa)
{
    *pa = 0;
    if (!p || p != live || p->state != PROC_FREE || (flags & ~(VMM_WRITE | VMM_EXEC)) ||
        va % PAGE_SIZE != 0 || va < USER_IMAGE_START || va >= USER_IMAGE_END ||
        p->npages >= PROC_IMAGE_PAGES)
        return PROC_EINVAL;
    return add_page(p, va, flags | VMM_USER, pa);
}

int proc_finish(struct process *p, uint64_t entry)
{
    uint64_t pa;
    int err;

    if (!p || p != live || p->state != PROC_FREE)
        return PROC_EINVAL;
    for (int i = 0; i < PROC_STACK_PAGES; i++)
        if ((err = add_page(p, USER_STACK_START + i * PAGE_SIZE, VMM_WRITE | VMM_USER, &pa)) != PROC_OK)
            return err;
    if (!verify_space(p))
        return PROC_ESTATE;
    struct vmm_mapping m;
    if (vmm_lookup(p->root, entry, &m) != VMM_OK || !(m.flags & VMM_EXEC) || !(m.flags & VMM_USER))
        return PROC_ESTATE;
    p->entry = entry;
    p->user_rsp = PROC_ENTRY_RSP;
    p->state = PROC_READY;
    return PROC_OK;
}

int proc_create(struct process *p, const char *name, const void *code, size_t len)
{
    uint64_t pa;
    int err;

    if (live)
        return PROC_EBUSY;
    if (!p || !code || len == 0 || len > PAGE_SIZE)
        return PROC_EINVAL;
    if ((err = proc_begin(p, name)) != PROC_OK)
        return err;
    if ((err = proc_add_page(p, PROC_CODE_VA, VMM_EXEC, &pa)) != PROC_OK)
        goto fail;
    memcpy(phys_to_virt(pa), code, len);
    if ((err = proc_finish(p, PROC_CODE_VA)) != PROC_OK)
        goto fail;
    return PROC_OK;

fail:
    proc_destroy(p);
    return err;
}

void proc_destroy(struct process *p)
{
    uint64_t pa;

    if (!p || p != live)
        panic("process: destroying an unknown process");
    if (p->state == PROC_RUNNING)
        panic("process: destroying a running process");
    if ((read_cr3() & PTE_ADDR) != vmm_kernel_root())
        panic("process: teardown while not on the kernel address space");

    for (unsigned i = 0; i < p->npages; i++) {
        if (vmm_unmap(p->root, p->page_va[i], &pa) != VMM_OK || pa != p->page_pa[i])
            panic("process: user page missing at teardown");
        must_free(pa, "process: frame release failed");
    }

    /* Every lower-half table must be gone; only the copied kernel entry remains. */
    uint64_t *l4 = phys_to_virt(p->root);
    struct vmm_audit a;
    if (vmm_audit_space(p->root, 1, &a) != VMM_OK || a.tables_lower || a.leaves_lower)
        panic("process: address space not empty at teardown");
    l4[511] = 0;
    for (int i = 0; i < 512; i++)
        if (l4[i])
            panic("process: root not empty at teardown");
    must_free(p->root, "process: root release failed");

    memset(p, 0, sizeof(*p));
    live = NULL;
}

int proc_run(struct process *p)
{
    uint64_t rflags;

    if (!p || p != live || p->state != PROC_READY || current)
        return PROC_ESTATE;
    if (!thread_is_boot())
        panic("process: user programs run only on the boot thread (N2.2)");
    __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(rflags));
    if (rflags & RFLAGS_IF)
        panic("process: interrupts enabled at user entry");
    if ((read_cr3() & PTE_ADDR) != vmm_kernel_root())
        panic("process: entry while not on the kernel address space");
    if (gdt_tss_rsp0() < KERNEL_VMA || gdt_tss_rsp0() % 16 || !gdt_stack_canaries_intact())
        panic("process: kernel-entry stack not ready");
    if (!verify_space(p))
        panic("process: address space failed verification before entry");

    p->state = PROC_RUNNING;
    p->tick_start = irq_ticks();
    current = p;
    int result = enter_user(p->entry, p->user_rsp, p->root, &p->kernel_rsp);
    current = NULL;

    if ((read_cr3() & PTE_ADDR) != vmm_kernel_root() || p->kernel_rsp != 0 ||
        (p->state != PROC_EXITED && p->state != PROC_KILLED) || result != p->state)
        panic("process: kernel resumed in an inconsistent state");
    if (!gdt_stack_canaries_intact())
        panic("process: kernel-entry stack canary overwritten");
    return result;
}

/* Leave ring 3 for good: back to the kernel root, then to proc_run's caller. */
__attribute__((noreturn)) static void finish(struct process *p)
{
    write_cr3(vmm_kernel_root());
    user_return(&p->kernel_rsp, p->state);
}

int proc_running(void)
{
    return current != NULL;
}

uint64_t proc_user_trap(struct exc_frame *f)
{
    struct process *p = current;

    if (!p || p->state != PROC_RUNNING || p->kernel_rsp == 0)
        panic("process: user trap with no running process");
    if (f->cs != GDT_USER_CS || f->ss != GDT_USER_DS)
        panic("process: user trap with unexpected selectors");
    if ((read_cr3() & PTE_ADDR) != p->root)
        panic("process: user trap on a foreign address space");

    if (irq_is_vector(f->vector)) {
        irq_handle(f);                  /* acknowledged before anything else happens */
        p->irqs++;
        p->ticks = irq_ticks() - p->tick_start;
        record(&p->last_irq, f);
#ifdef VOS_TEST
        /* Normally irqs == ticks (only IRQ0 is unmasked); the second bound catches a stuck counter. */
        if (tick_budget && (p->ticks >= tick_budget || p->irqs >= 2 * tick_budget + 16)) {
            record(&p->trap, f);
            p->state = PROC_KILLED;
            p->reason = p->ticks >= tick_budget ? PROC_REASON_BUDGET : PROC_REASON_IRQ_BUDGET;
            serial_puts(p->reason == PROC_REASON_BUDGET ? "proc: killed by the test tick budget after " :
                        "proc: killed by the test interrupt budget (ticks not advancing) after ");
            serial_dec(p->ticks);
            serial_puts(" timer ticks, ");
            serial_dec(p->irqs);
            serial_puts(" interrupts (cs=");
            serial_hex(f->cs);
            serial_puts(" rip=");
            serial_hex(f->rip);
            serial_puts(" rflags=");
            serial_hex(f->rflags);
            serial_puts(")\r\n");
            finish(p);
        }
#endif
        return resume(p, f);
    }
    if (f->vector == IDT_SYSCALL && f->rax != SYS_EXIT) {
        int64_t res = syscall_dispatch(p, f);
        f->rax = (uint64_t)res;
        if (res >= 0)
            p->sys_ok++;
        else if (res == -VOS_ENOSYS)
            p->sys_enosys++;
        else if (res == -VOS_EINVAL)
            p->sys_einval++;
        else if (res == -VOS_EFAULT)
            p->sys_efault++;
        p->syscalls++;
        return resume(p, f);
    }

    record(&p->trap, f);

    if (f->vector == IDT_SYSCALL) {             /* exit */
        p->exit_status = (int32_t)f->rdi;
        p->state = PROC_EXITED;
        serial_puts("proc: exit status ");
        serial_dec((uint32_t)p->exit_status);
        serial_puts(" (cs=");
        serial_hex(f->cs);
        serial_puts(" rip=");
        serial_hex(f->rip);
        serial_puts(")\r\n");
        finish(p);
    }
    /* A user exception ends the process only; the kernel continues. */
    p->state = PROC_KILLED;
    p->reason = exception_name(f->vector);
    serial_puts("proc: killed by ");
    serial_puts(p->reason);
    serial_puts(" err=");
    serial_hex(f->error);
    serial_puts(" (cs=");
    serial_hex(f->cs);
    serial_puts(" rip=");
    serial_hex(f->rip);
    if (f->vector == 14) {
        serial_puts(" cr2=");
        serial_hex(p->trap.cr2);
    }
    serial_puts(")\r\n");
    finish(p);
}
