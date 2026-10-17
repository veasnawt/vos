/*
 * Kernel threads and scheduler (N2.2), VOS_TEST only. Every verdict is the
 * kernel's own: the threads under test are trusted kernel code that record
 * what they observed in kernel memory, and the boot thread checks it.
 */
#include "selftest.h"
#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "kmap.h"
#include "lib.h"
#include "memlayout.h"
#include "thread.h"
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

/* ---- records and guarded stacks ---- */

static void test_stacks(void)
{
    uint64_t g[THREAD_STACKS], kg[KMAP_MAX_GUARDS];
    unsigned n = thread_guard_pages(g, THREAD_STACKS), nk = kmap_guard_pages(kg, KMAP_MAX_GUARDS);
    unsigned unmapped = 0, rw = 0, listed = 0, distinct = 1;
    struct vmm_mapping m;

    for (unsigned i = 0; i < n; i++) {
        unmapped += vmm_lookup(vmm_kernel_root(), g[i], &m) == VMM_ENOTMAPPED &&
                    vmm_lookup(vmm_kernel_root(), g[i] + THREAD_GUARD_SIZE - 8, &m) == VMM_ENOTMAPPED;
        /* the stack right above: 16 KiB, supervisor RW NX, 4 KiB pages */
        int ok = 1;
        for (uint64_t va = g[i] + THREAD_GUARD_SIZE; va < g[i] + THREAD_GUARD_SIZE + THREAD_STACK_SIZE; va += PAGE_SIZE)
            ok &= vmm_lookup(vmm_kernel_root(), va, &m) == VMM_OK && m.flags == VMM_WRITE &&
                  m.page_size == PAGE_SIZE;
        rw += ok;
        for (unsigned k = 0; k < nk; k++)
            listed += kg[k] == g[i];
        for (unsigned j = 0; j < i; j++)
            distinct &= g[j] != g[i] && (g[i] > g[j] ? g[i] - g[j] : g[j] - g[i]) >=
                                            THREAD_GUARD_SIZE + THREAD_STACK_SIZE;
        distinct &= g[i] % PAGE_SIZE == 0;
    }
    expect(n == THREAD_STACKS && unmapped == n && listed == n,
           "every thread stack has an unmapped guard page, listed in the kernel map's guards");
    expect(rw == n && distinct, "thread stacks: 16 KiB each, page aligned, disjoint, supervisor RW NX");

    struct thread *b = thread_current();
    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    expect(b && thread_is_boot() && b->id == 1 && b->state == THREAD_RUNNING && b->stack == -1 &&
           rsp > b->stack_lo && rsp <= b->stack_hi, "the boot code runs as thread 1 on the boot stack");
    expect(thread_canaries_intact() && gdt_tss_rsp0() > KERNEL_VMA &&
           !(gdt_tss_rsp0() > g[0] && gdt_tss_rsp0() <= g[n - 1] + THREAD_GUARD_SIZE + THREAD_STACK_SIZE),
           "canaries intact; TSS.RSP0 is the dedicated entry stack, never a thread stack");
}

int selftest_sched(void)
{
    failures = cases = 0;
    test_stacks();
    serial_puts("selftest: sched ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}
