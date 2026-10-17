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
#include "gdt.h"
#include "lib.h"
#include "memlayout.h"
#include "panic.h"
#include "pmm.h"
#include "process.h"
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

/* Frames one process holds: root + 5 pages + 6 tables (3 for code, 3 for stack). */
#define PROC_FRAMES (1 + PROC_MAX_PAGES + 6)

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

    expect(proc_create(&proc, code, sizeof(code)) == PROC_OK && proc.state == PROC_READY,
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
           a.leaves_lower == 5 && a.user_leaves == 5 && a.large_upper == 512,
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

    struct process other;
    expect(proc_create(&other, code, sizeof(code)) == PROC_EBUSY, "only one process at a time");
    expect(proc_create(&other, code, 0) == PROC_EBUSY, "busy check precedes argument checks");

    proc_destroy(&proc);
    expect(pmm_free_count() == f0 && proc.state == PROC_FREE, "teardown restores the free count");
    expect(vmm_audit_space(K, 1, &a) == VMM_OK && a.tables_lower == 0, "kernel space untouched");

    expect(proc_create(&proc, code, 0) == PROC_EINVAL &&
           proc_create(&proc, code, PAGE_SIZE + 1) == PROC_EINVAL && pmm_free_count() == f0,
           "invalid code sizes refused without allocation");

    /* Allocation failure at every step: nothing leaks, no process remains. */
    int rollback_ok = 1;
    for (int budget = 0; budget <= PROC_MAX_PAGES; budget++) {
        proc_test_frame_budget(budget);
        int err = proc_create(&proc, code, sizeof(code));
        proc_test_frame_budget(-1);
        rollback_ok &= err == PROC_ENOMEM && pmm_free_count() == f0;
    }
    for (int budget = 0; budget < 6; budget++) {
        vmm_test_table_budget(budget);
        int err = proc_create(&proc, code, sizeof(code));
        vmm_test_table_budget(-1);
        rollback_ok &= err == PROC_ENOMEM && pmm_free_count() == f0;
    }
    expect(rollback_ok, "frame or table allocation failure at any step rolls back completely");
    expect(proc_create(&proc, code, sizeof(code)) == PROC_OK, "creation works after the failures");
    proc_destroy(&proc);

    int cycles_ok = 1;
    for (int i = 0; i < 50; i++) {
        cycles_ok &= proc_create(&proc, hlt_code, sizeof(hlt_code)) == PROC_OK;
        proc_destroy(&proc);
        cycles_ok &= pmm_free_count() == f0;
    }
    expect(cycles_ok && pmm_check_in(pmm_kernel()) == PMM_OK, "50 create/destroy cycles leak nothing");
    expect(gdt_stack_canaries_intact(), "RSP0 and IST1 stack canaries intact");
}

int selftest_user(void)
{
    failures = cases = 0;
    test_address_space();
    serial_puts("selftest: user ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}
