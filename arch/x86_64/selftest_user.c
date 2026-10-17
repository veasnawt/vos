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
#include "kmap.h"
#include "lib.h"
#include "memlayout.h"
#include "panic.h"
#include "pmm.h"
#include "process.h"
#include "syscall.h"
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

    static struct process other;
    expect(proc_create(&other, "other", code, sizeof(code)) == PROC_EBUSY, "only one process at a time");
    expect(proc_create(&other, "other", code, 0) == PROC_EBUSY, "busy check precedes argument checks");

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
             last.trap.kframe + sizeof(struct exc_frame) == gdt_tss_rsp0();
    if (!ok) {
        serial_puts("selftest: run post-conditions failed for ");
        serial_puts(name);
        serial_puts("\r\n");
    }
    expect(ok, "run: CS 0x23/SS 0x1B frame at the top of RSP0, kernel resumed intact, no rerun, no leak");
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
    expect((last.trap.rflags & (RFLAGS_IF | RFLAGS_IOPL)) == 0 && (last.trap.rflags & 2),
           "saved RFLAGS: IF 0, IOPL 0");

    int r = run_prog("regcheck", PROG(regcheck), 0, 0);
    expect(r == PROC_EXITED && last.exit_status == 0x5A,
           "entry state: GPRs zero, RFLAGS 0x2, RSP 0x7FFFFFFFEFF0, aligned, 16 zero bytes");
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

int selftest_user(void)
{
    failures = cases = 0;
    test_address_space();
    test_copy_from_user();
    test_ring3_exit();
    test_debug_write();
    test_user_faults();
    serial_puts("selftest: user ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}
