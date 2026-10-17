#include "process.h"
#include "cpu.h"
#include "lib.h"
#include "panic.h"
#include "pmm.h"
#include "vmm.h"

/* The one process that may exist (created and not yet destroyed). */
static struct process *live;

#ifdef VOS_TEST
static int frame_budget = -1;

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
    if (p->npages >= PROC_MAX_PAGES)
        return PROC_EINVAL;
    if (frame_alloc(pa) != PMM_OK)
        return PROC_ENOMEM;
    int err = vmm_map(p->root, va, *pa, flags);
    if (err != VMM_OK) {
        must_free(*pa, "process: frame release failed");
        return err == VMM_ENOMEM ? PROC_ENOMEM : PROC_EINVAL;
    }
    p->page_va[p->npages] = va;
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

    if (l4[511] != kl4[511] || (l4[511] & PTE_U))
        return 0;
    for (int i = 256; i < 511; i++)
        if (l4[i])
            return 0;
    if (vmm_audit_space(p->root, 1, &a) != VMM_OK || a.leaves_lower != p->npages ||
        a.user_leaves != p->npages)
        return 0;
    if (vmm_lookup(p->root, PROC_CODE_VA, &m) != VMM_OK || m.flags != (VMM_EXEC | VMM_USER))
        return 0;
    for (int i = 0; i < PROC_STACK_PAGES; i++)
        if (vmm_lookup(p->root, USER_STACK_START + i * PAGE_SIZE, &m) != VMM_OK ||
            m.flags != (VMM_WRITE | VMM_USER))
            return 0;
    return vmm_lookup(p->root, 0, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(p->root, USER_STACK_GUARD, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(p->root, USER_STACK_END, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(p->root, PROC_CODE_VA + PAGE_SIZE, &m) == VMM_ENOTMAPPED;
}

int proc_create(struct process *p, const void *code, size_t len)
{
    uint64_t pa;
    int err;

    if (live)
        return PROC_EBUSY;
    if (!p || !code || len == 0 || len > PAGE_SIZE)
        return PROC_EINVAL;
    memset(p, 0, sizeof(*p));

    if (frame_alloc(&p->root) != PMM_OK)
        return PROC_ENOMEM;
    /* The kernel half: PML4[511] by value, nothing else. */
    ((uint64_t *)phys_to_virt(p->root))[511] =
        ((const uint64_t *)phys_to_virt(vmm_kernel_root()))[511];
    live = p;
    p->state = PROC_FREE;

    if ((err = add_page(p, PROC_CODE_VA, VMM_EXEC | VMM_USER, &pa)) != PROC_OK)
        goto fail;
    memcpy(phys_to_virt(pa), code, len);
    for (int i = 0; i < PROC_STACK_PAGES; i++)
        if ((err = add_page(p, USER_STACK_START + i * PAGE_SIZE, VMM_WRITE | VMM_USER, &pa)) != PROC_OK)
            goto fail;
    if (!verify_space(p)) {
        err = PROC_ESTATE;
        goto fail;
    }
    p->entry = PROC_CODE_VA;
    p->user_rsp = PROC_ENTRY_RSP;
    p->state = PROC_READY;
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
