/*
 * User address-space and ring-3 self-tests (VOS_TEST only).
 *
 * Every check uses values fixed here (frame counts, permissions, selectors,
 * error codes) and, for ring 3, CPU-generated evidence: the frame the CPU
 * pushed on the kernel-entry stack, #GP for privileged instructions and the
 * U bit of #PF error codes.
 */
#include "selftest.h"
#include "console.h"
#include "cpu.h"
#include "exception.h"
#include "gdt.h"
#include "irq.h"
#include "kmap.h"
#include "lib.h"
#include "memlayout.h"
#include "panic.h"
#include "pmm.h"
#include "process.h"
#include "syscall.h"
#include "thread.h"
#include "usercopy.h"
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

static struct process proc;
/* Process records shared by the multi-process tests (static: the kernel has no heap). */
static struct process procs[PROC_TABLE];

/* user_progs.S */
#define DECLARE_PROG(n) extern const uint8_t up_##n[], up_##n##_end[]
DECLARE_PROG(exit42);
DECLARE_PROG(regcheck);
DECLARE_PROG(enosys);
DECLARE_PROG(hello);
DECLARE_PROG(badptr);
DECLARE_PROG(forge);
DECLARE_PROG(many);
DECLARE_PROG(std_exit);
DECLARE_PROG(ud2);
DECLARE_PROG(cli);
DECLARE_PROG(hlt);
DECLARE_PROG(movcr3);
DECLARE_PROG(outb);
DECLARE_PROG(int14);
DECLARE_PROG(int81);
DECLARE_PROG(sgdt);
DECLARE_PROG(kwrite);
DECLARE_PROG(kread);
DECLARE_PROG(nullread);
DECLARE_PROG(spin);
DECLARE_PROG(kernelif);
DECLARE_PROG(sti);
DECLARE_PROG(iflock);
DECLARE_PROG(irqregs);
DECLARE_PROG(memtag);
DECLARE_PROG(codewrite);
DECLARE_PROG(execstack);
DECLARE_PROG(guard);
DECLARE_PROG(x87);
DECLARE_PROG(mmx);
DECLARE_PROG(sse);
#define PROG(n) up_##n, (size_t)(up_##n##_end - up_##n)
uint64_t call_check_callee_saved(int (*fn)(void *), void *arg, int *result);

#define RFLAGS_IF       (1ULL << 9)
#define RFLAGS_DF       (1ULL << 10)
#define RFLAGS_IOPL     (3ULL << 12)

/* Frames one process holds: root + 5 pages + 6 tables (3 for code, 3 for stack). */
#define PROC_FRAMES (1 + 1 + PROC_STACK_PAGES + 6)

static const uint8_t hlt_code[] = { 0xF4 };

static int all_zero_frame(uint64_t pa, size_t from)
{
    const uint8_t *b = phys_to_virt(pa);

    for (size_t i = from; i < PAGE_SIZE; i++)
        if (b[i])
            return 0;
    return 1;
}

static void test_address_space(void)
{
    const uint64_t K = vmm_kernel_root();
    const uint64_t *kl4 = phys_to_virt(K);
    uint64_t f0 = pmm_free_count();
    struct vmm_mapping m;
    struct vmm_audit a;
    static const uint8_t code[] = { 0x90, 0x90, 0xF4, 0xEB, 0xFE };

    expect(proc_create(&proc, "test", code, sizeof(code)) == PROC_OK && proc.state == PROC_READY,
           "process address space created");
    expect(pmm_free_count() == f0 - PROC_FRAMES, "root, 5 pages and 6 tables allocated");
    const uint64_t *l4 = phys_to_virt(proc.root);
    expect(l4[511] == kl4[511] && !(l4[511] & PTE_U), "PML4[511] copied by value, supervisor-only");
    int others = 0;
    for (int i = 1; i < 511; i++)
        if (i != 255 && l4[i])
            others++;
    expect(others == 0 && l4[0] && l4[255], "only PML4[0], [255] and [511] are present");
    expect(vmm_audit_space(proc.root, 1, &a) == VMM_OK && a.tables_lower == 6 &&
           a.leaves_lower == 5 && a.user_leaves == 5 && a.large_upper == KMAP_LARGE_PAGES,
           "audit: alias intact, 6 tables, 5 user leaves, no W+X");

    expect(vmm_lookup(proc.root, PROC_CODE_VA, &m) == VMM_OK && m.flags == (VMM_EXEC | VMM_USER) &&
           m.pa == proc.page_pa[0], "code page: user, read + execute, not writable");
    expect(memcmp(phys_to_virt(proc.page_pa[0]), code, sizeof(code)) == 0 &&
           all_zero_frame(proc.page_pa[0], sizeof(code)), "code copied; rest of the page zero");
    int stack_ok = 1;
    for (int i = 0; i < PROC_STACK_PAGES; i++)
        stack_ok &= vmm_lookup(proc.root, USER_STACK_START + i * PAGE_SIZE, &m) == VMM_OK &&
                    m.flags == (VMM_WRITE | VMM_USER) && all_zero_frame(m.pa, 0);
    expect(stack_ok, "stack pages: user, read + write, NX, zeroed");
    expect(vmm_lookup(proc.root, 0, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(proc.root, NULL_GUARD_END - PAGE_SIZE, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(proc.root, USER_STACK_GUARD, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(proc.root, USER_STACK_END, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(proc.root, PROC_CODE_VA + PAGE_SIZE, &m) == VMM_ENOTMAPPED,
           "null guard, stack guard, top page and code neighbour unmapped");
    expect(vmm_lookup(proc.root, KERNEL_VMA + PMM_LOW_LIMIT, &m) == VMM_OK && !(m.flags & VMM_USER),
           "kernel alias visible to the process root only as supervisor");
    expect(vmm_lookup(proc.root, (uint64_t)phys_to_virt(VGA_TEXT_PHYS), &m) == VMM_OK &&
           !(m.flags & VMM_USER), "no user-accessible VGA or low-memory mapping");
    expect(proc.entry == PROC_CODE_VA && proc.user_rsp == PROC_ENTRY_RSP &&
           proc.user_rsp % 16 == 0 && proc.user_rsp + 16 <= USER_STACK_END,
           "entry RIP and 16-byte aligned RSP with 16 bytes above it");

    /* N2.3: several processes may exist; each has its own root, frames and ID; the table is bounded. */
    static struct process other;
    struct process *extra = procs;
    uint64_t f1 = pmm_free_count();
    expect(proc_create(&other, "other", code, sizeof(code)) == PROC_OK && other.root != proc.root &&
           other.pid > proc.pid && other.page_pa[0] != proc.page_pa[0] && proc_count() == 2,
           "a second process gets its own root, frames and process ID");
    expect(proc_create(&proc, "again", code, sizeof(code)) == PROC_EINVAL && proc.state == PROC_READY,
           "an existing process cannot be begun again");
    unsigned made = 2;
    int full = PROC_OK;
    for (unsigned i = 0; i < PROC_TABLE && full == PROC_OK; i++) {
        full = proc_create(&extra[i], "fill", code, sizeof(code));
        made += full == PROC_OK;
    }
    uint64_t f2 = pmm_free_count();
    expect(made == PROC_TABLE && full == PROC_EBUSY && proc_create(&extra[PROC_TABLE - 1], "x", code, sizeof(code)) ==
           PROC_EBUSY && pmm_free_count() == f2, "the process table holds PROC_TABLE processes; the next is refused "
           "with PROC_EBUSY before any allocation");
    for (unsigned i = 0; i < made - 2; i++)
        proc_destroy(&extra[i]);
    proc_destroy(&other);
    expect(pmm_free_count() == f1 && proc_count() == 1, "destroying them returns every frame");

    proc_destroy(&proc);
    expect(pmm_free_count() == f0 && proc.state == PROC_FREE, "teardown restores the free count");
    expect(vmm_audit_space(K, 1, &a) == VMM_OK && a.tables_lower == 0, "kernel space untouched");

    expect(proc_create(&proc, "test", code, 0) == PROC_EINVAL &&
           proc_create(&proc, "test", code, PAGE_SIZE + 1) == PROC_EINVAL && pmm_free_count() == f0,
           "invalid code sizes refused without allocation");

    /* Allocation failure at every step: nothing leaks, no process remains. */
    int rollback_ok = 1;
    for (int budget = 0; budget < 2 + PROC_STACK_PAGES; budget++) {
        proc_test_frame_budget(budget);
        int err = proc_create(&proc, "test", code, sizeof(code));
        proc_test_frame_budget(-1);
        rollback_ok &= err == PROC_ENOMEM && pmm_free_count() == f0;
    }
    for (int budget = 0; budget < 6; budget++) {
        vmm_test_table_budget(budget);
        int err = proc_create(&proc, "test", code, sizeof(code));
        vmm_test_table_budget(-1);
        rollback_ok &= err == PROC_ENOMEM && pmm_free_count() == f0;
    }
    expect(rollback_ok, "frame or table allocation failure at any step rolls back completely");
    expect(proc_create(&proc, "test", code, sizeof(code)) == PROC_OK, "creation works after the failures");
    proc_destroy(&proc);

    int cycles_ok = 1;
    for (int i = 0; i < 50; i++) {
        cycles_ok &= proc_create(&proc, "cycle", hlt_code, sizeof(hlt_code)) == PROC_OK;
        proc_destroy(&proc);
        cycles_ok &= pmm_free_count() == f0;
    }
    expect(cycles_ok && pmm_check_in(pmm_kernel()) == PMM_OK, "50 create/destroy cycles leak nothing");
    expect(gdt_stack_canaries_intact(), "RSP0 and IST1 stack canaries intact");
}

/* ---- copy_from_user on a process address space (not running) ---- */

static uint8_t cbuf[300];

/* Copy into cbuf (pre-filled with 0xEE); returns the result. */
static int cfu(uint64_t uptr, size_t len)
{
    memset(cbuf, 0xEE, sizeof(cbuf));
    return copy_from_user(proc.root, cbuf, uptr, len);
}

static int cbuf_untouched_from(size_t from)
{
    for (size_t i = from; i < sizeof(cbuf); i++)
        if (cbuf[i] != 0xEE)
            return 0;
    return 1;
}

static void test_copy_from_user(void)
{
    static const uint8_t code[] = "copy-from-user code bytes";
    uint64_t f0 = pmm_free_count(), pa;
    struct vmm_mapping m;

    if (proc_create(&proc, "test", code, sizeof(code)) != PROC_OK) {
        expect(0, "process for copy_from_user tests");
        return;
    }
    /* Distinct bytes around the boundary between the first two stack pages. */
    uint8_t *s0 = phys_to_virt(proc.page_pa[1]), *s1 = phys_to_virt(proc.page_pa[2]);
    s0[PAGE_SIZE - 3] = 'a';
    s0[PAGE_SIZE - 2] = 'b';
    s0[PAGE_SIZE - 1] = 'c';
    s1[0] = 'd';
    s1[1] = 'e';
    s1[2] = 'f';
    uint8_t *top = phys_to_virt(proc.page_pa[proc.npages - 1]);
    top[PAGE_SIZE - 1] = 'z';

    expect(cfu(0, 0) == 0 && cbuf_untouched_from(0), "len 0: success, pointer not examined");
    expect(cfu(PROC_CODE_VA, sizeof(code)) == 0 && memcmp(cbuf, code, sizeof(code)) == 0 &&
           cbuf_untouched_from(sizeof(code)), "valid buffer copied exactly, nothing beyond len");
    expect(cfu(USER_STACK_START + PAGE_SIZE - 3, 6) == 0 && memcmp(cbuf, "abcdef", 6) == 0 &&
           cbuf_untouched_from(6), "range crossing two mapped pages copied");
    expect(cfu(USER_STACK_END - 1, 1) == 0 && cbuf[0] == 'z', "last mapped user byte copied");

    static const struct { uint64_t p; size_t n; const char *what; } bad[] = {
        { 0, 5, "null pointer" },
        { NULL_GUARD_END - 4, 2, "null guard" },
        { NULL_GUARD_END - 2, 4, "range starting in the null guard" },
        { 0x500000, 5, "unmapped page" },
        { PROC_CODE_VA + PAGE_SIZE - 2, 5, "range running into an unmapped page" },
        { USER_STACK_END - 1, 2, "range running into the unmapped top page" },
        { USER_STACK_GUARD + PAGE_SIZE - 1, 2, "range starting in the stack guard page" },
        { KERNEL_VMA + 0x100000, 5, "kernel-half address (mapped, supervisor)" },
        { 0xFFFFFFFFFFFFFF80ULL, 0x100, "range wrapping past 2^64" },
        { 0x0000800000000000ULL, 5, "first non-canonical address" },
        { 0x8000000000000000ULL, 5, "non-canonical address" },
        { 0x00007FFFFFFFFFF0ULL, 0x20, "range crossing into the non-canonical hole" },
        { PROC_CODE_VA, (size_t)-1, "length near 2^64" },
    };
    int bad_ok = 1;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int ok = cfu(bad[i].p, bad[i].n) == -VOS_EFAULT && cbuf_untouched_from(0);
        if (!ok) {
            serial_puts("selftest:   copy_from_user accepted: ");
            serial_puts(bad[i].what);
            serial_puts("\r\n");
        }
        bad_ok &= ok;
    }
    expect(bad_ok, "every invalid range refused with -VOS_EFAULT, nothing copied");

    /* A supervisor-only page inside the user range is refused. */
    uint64_t sup = 0, extra = 0;
    int setup_sup = pmm_alloc_frame(&sup) == PMM_OK && vmm_map(proc.root, 0x600000, sup, 0) == VMM_OK;
    expect(setup_sup, "setup: supervisor page in the process lower half");
    if (setup_sup) {
        ((uint8_t *)phys_to_virt(sup))[0] = 'S';
        expect(cfu(0x600000, 1) == -VOS_EFAULT && cbuf_untouched_from(0),
               "supervisor-only page in the lower half refused");
        vmm_unmap(proc.root, 0x600000, &pa);
    }
    /* A user leaf under an intermediate entry without U is refused (effective U is the AND). */
    int setup_extra = pmm_alloc_frame(&extra) == PMM_OK &&
                      vmm_map(proc.root, 0x800000, extra, VMM_USER) == VMM_OK;
    expect(setup_extra, "setup: extra user page under its own page table");
    if (setup_extra) {
        uint64_t *l4 = phys_to_virt(proc.root);
        uint64_t *pdpt = phys_to_virt(l4[0] & PTE_ADDR);
        uint64_t *pd = phys_to_virt(pdpt[0] & PTE_ADDR);
        expect(cfu(0x800000, 1) == 0, "user page readable while every level has U");
        pd[4] &= ~PTE_U;
        expect(cfu(0x800000, 1) == -VOS_EFAULT && vmm_lookup(proc.root, 0x800000, &m) == VMM_OK &&
               !(m.flags & VMM_USER), "user leaf under a supervisor PD entry refused");
        pd[4] |= PTE_U;
        vmm_unmap(proc.root, 0x800000, &pa);
    }
    if (sup)
        pmm_free_frame(sup);
    if (extra)
        pmm_free_frame(extra);
    proc_destroy(&proc);
    expect(pmm_free_count() == f0, "copy_from_user tests leave no frame behind");
}

/* ---- ring 3 ---- */

static struct process last;             /* copy of the process taken before teardown */

static int run_proc_fn(void *arg)
{
    return proc_run(arg);
}

/*
 * Create, (optionally) patch the argument slot, run, and destroy. Checks the
 * post-conditions every run must meet; returns the final state.
 */
static int run_prog(const char *name, const uint8_t *code, size_t len, int has_arg, uint64_t arg)
{
    uint64_t f0 = pmm_free_count();
    int result = -100;

    if (proc_create(&proc, name, code, len) != PROC_OK) {
        expect(0, name);
        return -100;
    }
    if (has_arg)
        *(uint64_t *)((uint8_t *)phys_to_virt(proc.page_pa[0]) + len - 8) = arg;
    uint64_t mismatch = call_check_callee_saved(run_proc_fn, &proc, &result);
    last = proc;
    int rerun = proc_run(&proc);
    proc_destroy(&proc);

    int ok = result == last.state && (result == PROC_EXITED || result == PROC_KILLED) &&
             rerun == PROC_ESTATE && mismatch == 0 && pmm_free_count() == f0 &&
             gdt_stack_canaries_intact() && (read_cr3() & PTE_ADDR) == vmm_kernel_root() &&
             last.trap.cs == GDT_USER_CS && last.trap.ss == GDT_USER_DS &&
             last.trap.kframe + sizeof(struct exc_frame) == last.kstack_top && last.kstack_top &&
             last.kstack_top != gdt_tss_rsp0();
    if (!ok) {
        serial_puts("selftest: run post-conditions failed for ");
        serial_puts(name);
        serial_puts("\r\n");
    }
    expect(ok, "run: CS 0x23/SS 0x1B frame at the top of the process thread's kernel stack, caller intact, "
               "no rerun, no leak");
    return result;
}

static int ended_by_int80(const struct process *p, const uint8_t *code, size_t len)
{
    uint64_t off = p->trap.rip - PROC_CODE_VA;

    return p->trap.vector == 0x80 && p->trap.rip >= PROC_CODE_VA + 2 && off <= len &&
           code[off - 2] == 0xCD && code[off - 1] == 0x80;
}

static void test_ring3_exit(void)
{
    uint64_t f0 = pmm_free_count();

    expect(run_prog("exit42", PROG(exit42), 0, 0) == PROC_EXITED && last.exit_status == 42,
           "ring 3 program exits with status 42");
    expect(ended_by_int80(&last, PROG(exit42)),
           "saved RIP follows the int $0x80 inside the user code page");
    expect(last.trap.rsp == PROC_ENTRY_RSP, "saved RSP is the user stack pointer");
    expect((last.trap.rflags & (RFLAGS_IF | RFLAGS_IOPL)) == RFLAGS_IF && (last.trap.rflags & 2),
           "saved RFLAGS: IF 1, IOPL 0");

    int r = run_prog("regcheck", PROG(regcheck), 0, 0);
    expect(r == PROC_EXITED && last.exit_status == 0x5A,
           "entry state: GPRs zero, RFLAGS 0x202, RSP 0x7FFFFFFFEFF0, aligned, 16 zero bytes");
    if (last.exit_status != 0x5A) {
        serial_puts("selftest:   regcheck status ");
        serial_dec((uint32_t)last.exit_status);
        serial_puts("\r\n");
    }

    r = run_prog("enosys", PROG(enosys), 0, 0);
    expect(r == PROC_EXITED && last.exit_status == 0 && last.syscalls == 2,
           "unknown system call returns -VOS_ENOSYS to the same process; all other state preserved");
    if (last.exit_status != 0) {
        serial_puts("selftest:   enosys status ");
        serial_dec((uint32_t)last.exit_status);
        serial_puts("\r\n");
    }
    expect(last.trap.rip > PROC_CODE_VA && ended_by_int80(&last, PROG(enosys)),
           "the process continued after both calls and exited from its last int $0x80");
    expect(run_prog("std_exit", PROG(std_exit), 0, 0) == PROC_EXITED && last.exit_status == 7 &&
           (last.trap.rflags & RFLAGS_DF), "user DF=1 at entry: kernel still correct");

    int cycles_ok = 1;
    for (int i = 0; i < 20; i++)
        cycles_ok &= run_prog("exit42", PROG(exit42), 0, 0) == PROC_EXITED && last.exit_status == 42;
    expect(cycles_ok && pmm_free_count() == f0 && pmm_check_in(pmm_kernel()) == PMM_OK,
           "20 ring-3 runs leak nothing");

    /* The canary check used after every run detects an overwritten word. */
    gdt_test_smash_canary(1);
    int detected = !gdt_stack_canaries_intact();
    gdt_test_smash_canary(0);
    expect(detected && gdt_stack_canaries_intact(), "RSP0 canary check detects corruption");
}

/* ---- debug_write ---- */

static char cap[512];
static size_t caplen;

static void cap_put(char c)
{
    if (caplen < sizeof(cap))
        cap[caplen++] = c;
}

static int emits(const char *data, size_t len, const char *want)
{
    caplen = 0;
    debug_emit("t", data, len, cap_put);
    return caplen == strlen(want) && memcmp(cap, want, caplen) == 0;
}

static void test_debug_write(void)
{
    uint64_t f0 = pmm_free_count();

    /* The sanitizer, exactly. */
    expect(emits("Hello\n", 6, "user[t]: Hello\r\n"), "emit: one line");
    expect(emits("no newline", 10, "user[t]: no newline\r\n"), "emit: line always terminated");
    expect(emits("a\n\nb", 4, "user[t]: a\r\nuser[t]: \r\nuser[t]: b\r\n"),
           "emit: every line, even an empty one, carries the prefix");
    expect(emits("ok\nVOS-TEST: PASS\r\n", 19, "user[t]: ok\r\nuser[t]: VOS-TEST: PASS?\r\n"),
           "emit: forged marker stays inside a prefixed line; CR becomes ?");
    expect(emits("\x1b[2J\x7f\xff\t\x00", 8, "user[t]: ?[2J????\r\n"),
           "emit: escape, DEL, non-ASCII, tab and NUL become ?");
    expect(emits("x", 0, ""), "emit: nothing for length 0");

    int r = run_prog("hello-asm", PROG(hello), 0, 0);
    expect(r == PROC_EXITED && last.exit_status == 0 && last.syscalls == 2,
           "debug_write returns its length to the same process, registers preserved, twice");
    if (last.exit_status != 0) {
        serial_puts("selftest:   hello status ");
        serial_dec((uint32_t)last.exit_status);
        serial_puts("\r\n");
    }
    r = run_prog("badptr", PROG(badptr), 0, 0);
    expect(r == PROC_EXITED && last.exit_status == 0 && last.syscalls == 12,
           "invalid debug_write arguments fail with -VOS_EFAULT/-VOS_EINVAL, process continues");
    if (last.exit_status != 0) {
        serial_puts("selftest:   badptr failed case ");
        serial_dec((uint32_t)last.exit_status);
        serial_puts("\r\n");
    }
    r = run_prog("forge", PROG(forge), 0, 0);
    expect(r == PROC_EXITED && last.exit_status == 0, "forging attempt accepted only as user output");
    r = run_prog("many", PROG(many), 0, 0);
    expect(r == PROC_EXITED && last.exit_status == 0 && last.syscalls == 1100,
           "1100 system calls in one process");
    expect(pmm_free_count() == f0 && gdt_stack_canaries_intact() && pmm_check_in(pmm_kernel()) == PMM_OK,
           "system calls leak nothing; kernel-entry stack intact");
}

/* ---- user faults end the process, never the kernel ---- */

#define PF_P    0x1
#define PF_W    0x2
#define PF_U    0x4
#define PF_ID   0x10

/* A present, supervisor-only kernel page the user programs aim at. */
static volatile uint64_t kernel_target = 0x5EC0DE5EC0DE5EC0ULL;

static int killed_by(uint64_t vector, uint64_t error)
{
    return last.state == PROC_KILLED && last.trap.vector == vector && last.trap.error == error &&
           last.trap.cs == GDT_USER_CS && last.trap.ss == GDT_USER_DS && last.reason;
}

static int rip_in_code(void)
{
    return last.trap.rip >= PROC_CODE_VA && last.trap.rip < PROC_CODE_VA + PAGE_SIZE;
}

static void test_user_faults(void)
{
    uint64_t f0 = pmm_free_count();
    uint64_t target = (uint64_t)&kernel_target;

    run_prog("ud2", PROG(ud2), 0, 0);
    expect(killed_by(6, 0) && last.trap.rip == PROC_CODE_VA, "user ud2: #UD at the program's first byte");

    /* Privileged instructions: #GP only because CPL is 3 (they succeed at CPL 0). */
    run_prog("cli", PROG(cli), 0, 0);
    expect(killed_by(13, 0) && last.trap.rip == PROC_CODE_VA, "user cli: #GP(0), CS 0x23");
    run_prog("hlt", PROG(hlt), 0, 0);
    expect(killed_by(13, 0) && rip_in_code(), "user hlt: #GP(0)");
    run_prog("movcr3", PROG(movcr3), 0, 0);
    expect(killed_by(13, 0) && rip_in_code(), "user mov %cr3: #GP(0)");
    run_prog("outb", PROG(outb), 0, 0);
    expect(killed_by(13, 0) && rip_in_code(), "user out to COM1: #GP(0) (IOPL 0, no I/O bitmap)");

    /* Software interrupts: only vector 0x80 is reachable from ring 3. */
    run_prog("int14", PROG(int14), 0, 0);
    expect(killed_by(13, 14 * 8 + 2) && rip_in_code(), "user int $14: #GP(0x72), no forged #PF");
    run_prog("int81", PROG(int81), 0, 0);
    expect(killed_by(13, 0x81 * 8 + 2) && rip_in_code(), "user int $0x81: #GP(0x40a)");

    /* Memory protection. */
    run_prog("kwrite", PROG(kwrite), 1, target);
    expect(killed_by(14, PF_P | PF_W | PF_U) && last.trap.cr2 == target && rip_in_code(),
           "user write to a kernel page: #PF P=1 W=1 U=1, CR2 = target");
    expect(kernel_target == 0x5EC0DE5EC0DE5EC0ULL, "kernel page unchanged by the user write");
    run_prog("kread", PROG(kread), 1, target);
    expect(killed_by(14, PF_P | PF_U) && last.trap.cr2 == target, "user read of a kernel page: #PF P=1 U=1");
    run_prog("kread-image", PROG(kread), 1, KERNEL_VMA + PMM_LOW_LIMIT);
    expect(killed_by(14, PF_P | PF_U) && last.trap.cr2 == KERNEL_VMA + PMM_LOW_LIMIT,
           "user read of the kernel image start (2 MiB alias page): #PF P=1 U=1");
    run_prog("nullread", PROG(nullread), 0, 0);
    expect(killed_by(14, PF_U) && last.trap.cr2 == 0, "user null read: #PF P=0 U=1, CR2 0");
    run_prog("codewrite", PROG(codewrite), 0, 0);
    expect(killed_by(14, PF_P | PF_W | PF_U) && last.trap.cr2 == PROC_CODE_VA,
           "user write to its own code: #PF P=1 W=1 U=1");
    run_prog("execstack", PROG(execstack), 0, 0);
    expect(killed_by(14, PF_P | PF_U | PF_ID) && last.trap.cr2 == PROC_ENTRY_RSP &&
           last.trap.rip == PROC_ENTRY_RSP, "user jump to the stack: #PF P=1 U=1 I/D=1 (NX)");
    run_prog("guard", PROG(guard), 0, 0);
    expect(killed_by(14, PF_W | PF_U) && last.trap.cr2 == USER_STACK_START - 8,
           "user push below the stack: #PF in the guard page");

    /* UMIP: with it, sgdt from ring 3 is #GP; without it, sgdt works (and the program exits 1). */
    run_prog("sgdt", PROG(sgdt), 0, 0);
    if (read_cr4() & CR4_UMIP)
        expect(killed_by(13, 0), "UMIP on: user sgdt raises #GP(0)");
    else
        expect(last.state == PROC_EXITED && last.exit_status == 1, "UMIP off: user sgdt executes");

    /* No FPU for user programs (ABI v0): the CPU enforces it through CR0.EM. */
    expect((read_cr0() & CR0_EM) && !(read_cr0() & (CR0_MP | CR0_TS)), "CR0.EM = 1, MP = 0, TS = 0");
    run_prog("x87", PROG(x87), 0, 0);
    expect(killed_by(7, 0) && last.trap.rip == PROC_CODE_VA, "user fld1: #NM, process killed");
    run_prog("mmx", PROG(mmx), 0, 0);
    expect(killed_by(6, 0) && last.trap.rip == PROC_CODE_VA, "user MMX pxor: #UD");
    run_prog("sse", PROG(sse), 0, 0);
    expect(killed_by(6, 0) && last.trap.rip == PROC_CODE_VA, "user xorps: #UD");

    /* The kernel launches a normal program after all of these. */
    expect(run_prog("exit42", PROG(exit42), 0, 0) == PROC_EXITED && last.exit_status == 42,
           "normal program runs after user faults");
    expect(pmm_free_count() == f0 && pmm_check_in(pmm_kernel()) == PMM_OK && gdt_stack_canaries_intact(),
           "no leak and intact canaries after every user fault");
}

static int budget_kill(uint64_t ticks)
{
    return last.state == PROC_KILLED && last.reason == PROC_REASON_BUDGET && last.ticks == ticks &&
           last.irqs >= ticks && last.trap.vector == IRQ_VECTOR_BASE + IRQ_TIMER &&
           last.trap.cs == GDT_USER_CS && last.trap.ss == GDT_USER_DS &&
           (last.trap.rflags & (RFLAGS_IF | RFLAGS_IOPL)) == RFLAGS_IF;
}

/*
 * The checks resume() makes before IRETQ to ring 3 (after a system call or an
 * IRQ), on crafted frames placed exactly where the CPU puts them: the top of
 * the idle TSS.RSP0 stack. Real frames never violate them, so this is the
 * only way to show that each check exists.
 */
static int frame_refused(struct process *p, struct exc_frame *f, const struct exc_frame *good,
                         uint64_t *field, uint64_t bad)
{
    *f = *good;
    *field = bad;
    const char *why = proc_test_frame_problem(p, f);
    *f = *good;
    return why != NULL;
}

static void test_resume_validation(void)
{
    static struct process fake;
    struct exc_frame *f = (struct exc_frame *)(gdt_tss_rsp0() - sizeof(struct exc_frame));
    struct exc_frame good, copy;

    memset(&fake, 0, sizeof(fake));
    fake.root = read_cr3() & PTE_ADDR;
    fake.kstack_top = gdt_tss_rsp0();   /* the idle entry stack plays the process's kernel stack */
    memset(&good, 0, sizeof(good));
    good.cs = GDT_USER_CS;
    good.ss = GDT_USER_DS;
    good.rflags = 0x202;
    good.rip = PROC_CODE_VA;
    good.rsp = PROC_ENTRY_RSP;
    *f = good;
    expect(proc_test_frame_problem(&fake, f) == NULL, "resume check accepts a valid user frame at the top of RSP0");
    expect(frame_refused(&fake, f, &good, &f->rflags, 0x2), "resume check refuses IF=0");
    expect(frame_refused(&fake, f, &good, &f->rflags, 0x3202), "resume check refuses IOPL=3");
    expect(frame_refused(&fake, f, &good, &f->rflags, 0x1202), "resume check refuses IOPL=1");
    expect(frame_refused(&fake, f, &good, &f->rflags, 0x20202), "resume check refuses VM=1");
    expect(frame_refused(&fake, f, &good, &f->rflags, 0x200), "resume check refuses a cleared fixed bit 1");
    expect(frame_refused(&fake, f, &good, &f->cs, GDT_KERNEL_CS), "resume check refuses the kernel CS");
    expect(frame_refused(&fake, f, &good, &f->ss, GDT_KERNEL_DS), "resume check refuses the kernel SS");
    expect(frame_refused(&fake, f, &good, &f->rip, KERNEL_VMA), "resume check refuses a kernel RIP");
    expect(frame_refused(&fake, f, &good, &f->rsp, LOWER_HALF_END), "resume check refuses a non-lower-half RSP");
    copy = good;
    expect(proc_test_frame_problem(&fake, &copy) != NULL, "resume check refuses a frame not at the top of RSP0");
    fake.root = 0x1000;
    expect(proc_test_frame_problem(&fake, f) != NULL, "resume check refuses a foreign address space");
    memset(f, 0, sizeof(*f));
}

/*
 * N2.1: hardware interrupts delivered by the CPU while ring 3 runs (IF = 1).
 * The tick budget is a test-only kill switch; nothing else ends these programs.
 */
static void test_irq_ring3(void)
{
    const struct irq_stats *st = irq_stats();
    uint64_t f0 = pmm_free_count(), t0 = irq_ticks(), unexpected0 = st->unexpected;
    size_t spin_len = (size_t)(up_spin_end - up_spin), regs_len = (size_t)(up_irqregs_end - up_irqregs);

    expect(!(read_rflags() & RFLAGS_IF), "kernel runs with IF=0 before entering ring 3");
    proc_test_tick_budget(5);
    expect(run_prog("spin", PROG(spin), 0, 0) == PROC_KILLED && budget_kill(5) && irq_ticks() - t0 >= 5,
           "endless ring-3 loop: 5 timer IRQs delivered from CPL 3, killed by the test tick budget");
    expect(last.trap.rip >= PROC_CODE_VA && last.trap.rip < PROC_CODE_VA + spin_len &&
           last.trap.rsp == PROC_ENTRY_RSP && last.trap.error == 0,
           "interrupted frame: RIP inside the loop, the user RSP, no error code, IF 1, IOPL 0");
    expect(!(read_rflags() & RFLAGS_IF), "kernel back with IF=0 after the budget kill");

    proc_test_tick_budget(20);
    int r = run_prog("irqregs", PROG(irqregs), 0, 0);
    expect(r == PROC_KILLED && budget_kill(20) && last.trap.rip >= PROC_CODE_VA &&
           last.trap.rip < PROC_CODE_VA + regs_len,
           "15 registers and DF intact across 20 timer interrupts (the program checks continuously)");
    if (r == PROC_EXITED) {
        serial_puts("selftest:   irqregs status ");
        serial_dec((uint32_t)last.exit_status);
        serial_puts("\r\n");
    }
    r = run_prog("iflock", PROG(iflock), 0, 0);
    expect(r == PROC_KILLED && budget_kill(20),
           "popfq cannot clear IF or raise IOPL in ring 3: interrupts still arrive");
    if (r == PROC_EXITED) {
        serial_puts("selftest:   iflock status ");
        serial_dec((uint32_t)last.exit_status);
        serial_puts("\r\n");
    }
    proc_test_tick_budget(PROC_TEST_TICK_BUDGET);

    expect(run_prog("kernelif", PROG(kernelif), 0, 0) == PROC_EXITED && last.exit_status == 0 &&
           last.sys_ok == 1, "kernel IF=0 inside a system call made by an IF=1 program");
    expect(run_prog("sti", PROG(sti), 0, 0) == PROC_KILLED && killed_by(13, 0) &&
           last.trap.rip == PROC_CODE_VA, "user sti: #GP(0)");
    expect(run_prog("exit42", PROG(exit42), 0, 0) == PROC_EXITED && last.exit_status == 42,
           "normal program after timer activity");
    expect(st->unexpected == unexpected0 && irq_chip()->masks() == 0xFFFE && irq_chip()->in_service() == 0,
           "no IRQ on a masked line; masks unchanged; nothing left in service at the controller");
    expect(pmm_free_count() == f0 && pmm_check_in(pmm_kernel()) == PMM_OK && gdt_stack_canaries_intact(),
           "no leak and intact canaries after the interrupt tests");
    serial_puts("selftest: ring-3 timer interrupts ");
    serial_dec(irq_ticks() - t0);
    serial_puts(" ticks, ");
    serial_dec(st->delivered[IRQ_TIMER]);
    serial_puts(" IRQ0 delivered since boot, spurious IRQ7/15 ");
    serial_dec(st->spurious[7]);
    serial_puts("/");
    serial_dec(st->spurious[15]);
    serial_puts("\r\n");
}

/*
 * N2.3: user programs run on their own process threads, so any kernel thread
 * may start and wait for one (N2.2 allowed only the boot thread).
 */
static struct process wproc;
static int wresult, wstatus;
static uint64_t wtop, wtid;

static void run_from_worker(void *arg)
{
    (void)arg;
    wresult = -100;
    if (proc_create(&wproc, "worker-run", PROG(exit42)) != PROC_OK)
        return;
    wresult = proc_run(&wproc);
    wstatus = wproc.exit_status;
    wtop = wproc.kstack_top;
    wtid = wproc.tid;
    proc_destroy(&wproc);
}

static void test_process_threads(void)
{
    uint64_t f0 = pmm_free_count();
    int64_t id = thread_create("runner", run_from_worker, 0);

    wresult = 0;
    for (int k = 0; k < 100 && thread_state((uint64_t)id) != THREAD_FREE; k++)
        thread_yield();
    expect(id > 0 && wresult == PROC_EXITED && wstatus == 42 && thread_state(wtid) == THREAD_FREE &&
           wtop != gdt_tss_rsp0() && pmm_free_count() == f0 && proc_count() == 0,
           "a kernel worker thread runs a process on its own thread and destroys it; nothing leaks");
    expect(gdt_tss_rsp0() == gdt_entry_stack_top() && (read_cr3() & PTE_ADDR) == vmm_kernel_root(),
           "on a kernel thread: TSS.RSP0 is the entry stack and CR3 the kernel root");
}

/* ---- N2.3: several processes at once, timer preemption ---- */

static int streq(const char *x, const char *y)
{
    return x && y && strlen(x) == strlen(y) && memcmp(x, y, strlen(x)) == 0;
}

static int start(struct process *p, const char *name, const uint8_t *code, size_t len, int has_arg, uint64_t arg)
{
    if (proc_create(p, name, code, len) != PROC_OK)
        return 0;
    if (has_arg)
        *(uint64_t *)((uint8_t *)phys_to_virt(p->page_pa[0]) + len - 8) = arg;
    return proc_start(p) == PROC_OK;
}

/* Wait for p, keep a copy of its record in *out, destroy it. */
static int finish(struct process *p, struct process *out)
{
    int st = proc_wait(p);

    *out = *p;
    proc_destroy(p);
    return st;
}

static struct process r[3];

static void test_preemption(void)
{
    uint64_t f0 = pmm_free_count();
    struct process *a = &procs[0], *b = &procs[1], *c = &procs[2];

    proc_test_tick_budget(300);         /* bound every case; the kills below end them much earlier */

    /* An endless loop next to a normal program: the normal one still finishes. */
    int ok = start(a, "spin", PROG(spin), 0, 0) && start(b, "exit42", PROG(exit42), 0, 0);
    int sb = finish(b, &r[1]);
    proc_kill(a, "test kill");
    int sa = finish(a, &r[0]);
    expect(ok && sb == PROC_EXITED && r[1].exit_status == 42 && sa == PROC_KILLED &&
           streq(r[0].reason, "test kill") && r[0].preemptions > 0,
           "an endless loop is preempted: a normal program next to it finishes; the loop is killed on demand");

    /* Three endless loops for 30 ticks: each gets a fair share. */
    ok = start(a, "spin-a", PROG(spin), 0, 0) && start(b, "spin-b", PROG(spin), 0, 0) &&
         start(c, "spin-c", PROG(spin), 0, 0);
    uint64_t t0 = irq_ticks();
    thread_sleep(30);
    uint64_t span = irq_ticks() - t0;
    proc_kill(a, "test kill");
    proc_kill(b, "test kill");
    proc_kill(c, "test kill");
    finish(a, &r[0]);
    finish(b, &r[1]);
    finish(c, &r[2]);
    uint64_t lo = ~0ULL, hi = 0, sum = 0;
    for (int i = 0; i < 3; i++) {
        lo = r[i].ticks < lo ? r[i].ticks : lo;
        hi = r[i].ticks > hi ? r[i].ticks : hi;
        sum += r[i].ticks;
    }
    serial_puts("selftest: round robin over ");
    serial_dec(span);
    serial_puts(" ticks: ");
    for (int i = 0; i < 3; i++) {
        serial_dec(r[i].ticks);
        serial_puts(i < 2 ? "/" : " ticks, preemptions ");
    }
    for (int i = 0; i < 3; i++) {
        serial_dec(r[i].preemptions);
        serial_puts(i < 2 ? "/" : "\r\n");
    }
    expect(ok && span >= 30 && lo >= 5 && hi - lo <= 2 * SCHED_SLICE_TICKS + 2 && sum + 6 >= span,
           "three endless loops share the CPU round robin (every one runs, shares within two slices)");

    /* Registers survive preemption between processes; memory stays private. */
    ok = start(a, "irqregs", PROG(irqregs), 0, 0) && start(b, "tag-1", PROG(memtag), 1, 0x1111111111111111ULL) &&
         start(c, "tag-2", PROG(memtag), 1, 0x2222222222222222ULL);
    thread_sleep(30);
    proc_kill(a, "test kill");
    proc_kill(b, "test kill");
    proc_kill(c, "test kill");
    finish(a, &r[0]);
    finish(b, &r[1]);
    finish(c, &r[2]);
    expect(ok && streq(r[0].reason, "test kill") && r[0].preemptions > 0,
           "15 registers and DF survive preemption by other processes (irqregs never exited)");
    expect(streq(r[1].reason, "test kill") && streq(r[2].reason, "test kill") && r[1].preemptions > 0 &&
           r[2].preemptions > 0, "two processes write the same user address: each only ever sees its own data");
    expect(r[0].kstack_top != r[1].kstack_top && r[1].kstack_top != r[2].kstack_top &&
           r[0].kstack_top != r[2].kstack_top && r[1].root != r[2].root && r[1].traps > 0 &&
           r[1].last_irq.kframe + sizeof(struct exc_frame) == r[1].kstack_top &&
           r[2].last_irq.kframe + sizeof(struct exc_frame) == r[2].kstack_top,
           "each process trapped on its own kernel stack (TSS.RSP0) in its own address space");

    /* One process faults, one exits, one keeps running. */
    ok = start(a, "spin", PROG(spin), 0, 0) && start(b, "null", PROG(nullread), 0, 0) &&
         start(c, "exit42", PROG(exit42), 0, 0);
    int s1 = finish(b, &r[1]), s2 = finish(c, &r[2]);
    int alive = a->state == PROC_RUNNING;
    proc_kill(a, "test kill");
    finish(a, &r[0]);
    expect(ok && s1 == PROC_KILLED && r[1].trap.vector == 14 && s2 == PROC_EXITED && r[2].exit_status == 42 &&
           alive && streq(r[0].reason, "test kill"),
           "a faulting process dies alone; one exits normally; the third keeps running until killed");

    /* Killing a process that has not run yet, and repeated creation and destruction. */
    ok = start(a, "spin", PROG(spin), 0, 0);
    proc_kill(a, "killed before entry");
    expect(ok && finish(a, &r[0]) == PROC_KILLED && r[0].traps == 0, "a process killed before it ran never enters ring 3");
    int cycles = 1;
    for (int i = 0; i < 15; i++) {
        cycles &= start(a, "exit42", PROG(exit42), 0, 0) && start(b, "exit42", PROG(exit42), 0, 0);
        cycles &= finish(a, &r[0]) == PROC_EXITED && finish(b, &r[1]) == PROC_EXITED;
    }
    proc_test_tick_budget(PROC_TEST_TICK_BUDGET);
    expect(cycles && pmm_free_count() == f0 && pmm_check_in(pmm_kernel()) == PMM_OK && proc_count() == 0 &&
           thread_count() == 2, "15 rounds of two concurrent processes: every frame, record and thread returns");
}

static struct wait_queue hold;

static void holder(void *arg)
{
    (void)arg;
    thread_block(&hold);
}

/* proc_start needs a thread: a full thread pool is a clean error, and the process can start later. */
static void test_thread_exhaustion(void)
{
    int64_t id[THREAD_MAX];
    unsigned n = 0;
    uint64_t f0 = pmm_free_count();

    memset(&hold, 0, sizeof(hold));
    while (n < THREAD_MAX && (id[n] = thread_create("holder", holder, 0)) > 0)
        n++;
    thread_yield();                     /* let them all block */
    int ok = proc_create(&procs[0], "exit42", PROG(exit42)) == PROC_OK;
    int err = proc_start(&procs[0]);
    int ready = procs[0].state == PROC_READY;
    thread_wake_all(&hold);
    for (int k = 0; k < 50 && thread_count() > 2; k++)
        thread_yield();
    int run = proc_run(&procs[0]);
    int status = procs[0].exit_status;
    proc_destroy(&procs[0]);
    expect(ok && n == THREAD_MAX - 2 && err == PROC_ENOMEM && ready && run == PROC_EXITED && status == 42 &&
           pmm_free_count() == f0, "a full thread pool makes proc_start fail with PROC_ENOMEM; it starts later");
}

int selftest_user(void)
{
    failures = cases = 0;
    test_address_space();
    test_copy_from_user();
    test_ring3_exit();
    test_debug_write();
    test_user_faults();
    test_resume_validation();
    test_irq_ring3();
    test_process_threads();
    test_preemption();
    test_thread_exhaustion();
    serial_puts("selftest: user ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}
