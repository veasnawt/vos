#include "pmm.h"
#include "lib.h"
#include "panic.h"

/*
 * R4-M3: pmm_init_in and its helpers (pmm.h ABI unchanged) are Rust: kernel/rust/mm/pmm_init.rs behind
 * arch/x86_64/rust/pmm_init.rs. The accepted C is tests/reference/pmm-r4m3.c.
 */

/*
 * R4-M2: pmm_alloc_in, pmm_free_in (its mutation and state update after pmm_free_check_in) and pmm_reserve_in (pmm.h ABI
 * unchanged) are Rust: kernel/rust/mm/pmm_transition.rs behind arch/x86_64/rust/pmm_transition.rs. The accepted C is
 * tests/reference/pmm-r4m2.c.
 */

/*
 * R4-M1: pmm_free_check_in, pmm_frame_state_in and pmm_check_in (the read-only checks, pmm.h ABI unchanged) are
 * Rust: kernel/rust/mm/pmm.rs behind arch/x86_64/rust/pmm.rs. The accepted C is tests/reference/pmm-r4m1.c.
 */

/* ---- the kernel allocator ---- */

/* In .bss, hence inside the reserved kernel image: the bitmaps can never be handed out. */
static uint64_t kernel_avail[PMM_WORDS(PMM_MAX_FRAMES)];
static uint64_t kernel_alloc[PMM_WORDS(PMM_MAX_FRAMES)];
static struct pmm kernel_pmm;

int pmm_init(const struct boot_info *bi, const struct resv_table *t)
{
    if (kernel_pmm.state != PMM_UNINIT)
        return PMM_EINVAL;
    return pmm_init_in(&kernel_pmm, kernel_avail, kernel_alloc, PMM_MAX_FRAMES, bi, t);
}

int pmm_alloc_frame(uint64_t *phys)
{
    int err = pmm_alloc_in(&kernel_pmm, phys);

    if (err == PMM_ECORRUPT)
        panic("pmm: allocator state corrupt");
    if (err == PMM_OK)
        memset(phys_to_virt(*phys), 0, PAGE_SIZE);      /* *phys < 1 GiB: mapped */
    return err;
}

int pmm_free_frame(uint64_t phys)
{
    int err = pmm_free_check_in(&kernel_pmm, phys);

    if (err == PMM_ECORRUPT)
        panic("pmm: allocator state corrupt");
    if (err != PMM_OK)
        return err;
#ifdef VOS_TEST
    memset(phys_to_virt(phys), PMM_POISON, PAGE_SIZE);  /* still owned by the caller */
#endif
    return pmm_free_in(&kernel_pmm, phys);
}

int pmm_reserve_frame(uint64_t phys)
{
    int err = pmm_reserve_in(&kernel_pmm, phys);

    if (err == PMM_ECORRUPT)
        panic("pmm: allocator state corrupt");
    return err;     /* the frame is neither zeroed nor touched: firmware may own it */
}

uint64_t pmm_free_count(void)
{
    return kernel_pmm.free;
}

const struct pmm *pmm_kernel(void)
{
    return &kernel_pmm;
}

const char *pmm_strerror(int err)
{
    static const char *const names[] = {
        "ok", "not ready", "out of memory", "unaligned address", "address out of range",
        "protected frame", "frame not allocated", "memory map unsupported",
        "invalid argument", "allocator state corrupt", "frame already allocated",
    };

    return err >= 0 && err < (int)(sizeof(names) / sizeof(names[0])) ? names[err] : "?";
}
