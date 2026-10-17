#include "loader.h"
#include "lib.h"
#include "memlayout.h"
#include "vmm.h"

int load_elf(struct process *p, const char *name, const uint8_t *img, uint64_t size,
             struct elf_image *im, int *elf_err)
{
    int err;

    *elf_err = elf_validate(img, size, im);
    if (*elf_err != ELF_OK)
        return PROC_EEXEC;              /* nothing allocated */
    if ((err = proc_begin(p, name)) != PROC_OK)
        return err;

    for (unsigned s = 0; s < im->nload; s++) {
        const struct elf_segment *seg = &im->load[s];
        unsigned flags = ((seg->flags & ELF_PF_W) ? VMM_WRITE : 0) |
                         ((seg->flags & ELF_PF_X) ? VMM_EXEC : 0);

        /* Validated: page-aligned vaddr, [vaddr, vaddr + memsz) in the user image
         * region, [offset, offset + filesz) inside the image, filesz <= memsz. */
        for (uint64_t off = 0; off < seg->memsz; off += PAGE_SIZE) {
            uint64_t pa;

            if ((err = proc_add_page(p, seg->vaddr + off, flags, &pa)) != PROC_OK)
                goto fail;
            if (off < seg->filesz) {
                uint64_t n = seg->filesz - off < PAGE_SIZE ? seg->filesz - off : PAGE_SIZE;
                memcpy(phys_to_virt(pa), img + seg->offset + off, n);
            }
        }
    }
    if ((err = proc_finish(p, im->entry)) != PROC_OK)
        goto fail;
    return PROC_OK;

fail:
    proc_destroy(p);
    return err;
}
