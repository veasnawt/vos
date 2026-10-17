#include "usercopy.h"
#include "lib.h"
#include "memlayout.h"
#include "panic.h"
#include "syscall.h"
#include "vmm.h"

/* The frame behind one user byte, if user-readable as specified; 0 otherwise. */
static int user_page(uint64_t root, uint64_t va, struct vmm_mapping *m)
{
    return vmm_lookup(root, va, m) == VMM_OK && m->page_size == PAGE_SIZE &&
           (m->flags & VMM_USER) && m->pa < BOOT_MAP_LIMIT;
}

int copy_from_user(uint64_t root, void *dst, uint64_t uptr, size_t len)
{
    struct vmm_mapping m;

    if (len == 0)
        return 0;
    if (uptr < USER_IMAGE_START || uptr >= LOWER_HALF_END || len > LOWER_HALF_END - uptr)
        return -VOS_EFAULT;

    uint64_t end = uptr + len;          /* <= LOWER_HALF_END: no overflow */
    for (uint64_t page = uptr & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE)
        if (!user_page(root, page, &m))
            return -VOS_EFAULT;

    for (uint64_t va = uptr; va < end;) {
        uint64_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));

        if (chunk > end - va)
            chunk = end - va;
        if (!user_page(root, va, &m))
            panic("copy_from_user: mapping changed during the copy");
        memcpy((uint8_t *)dst + (va - uptr), phys_to_virt(m.pa), chunk);
        va += chunk;
    }
    return 0;
}
