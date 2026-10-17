/* Loading a VOS ABI v0 executable into a new process (N1.8). */
#ifndef VOS_X86_64_LOADER_H
#define VOS_X86_64_LOADER_H

#include <stdint.h>

#include "elf.h"
#include "process.h"

#define PROC_EEXEC  (-5)                /* the executable failed validation */

/*
 * Validate the image (elf_validate, before any allocation; *elf_err gets its
 * result), then build the process: every PT_LOAD page is a fresh zeroed frame
 * receiving exactly the segment's file bytes (the rest stays zero), mapped
 * user-accessible with W only for PF_W and X only for PF_X; then the stack,
 * the entry point and the full address-space check (proc_finish). The image
 * is read through the kernel alias and is never mapped into the process.
 * On any failure everything allocated is released and no process remains.
 * Returns PROC_OK, PROC_EEXEC, PROC_EBUSY or PROC_ENOMEM/PROC_ESTATE.
 */
int load_elf(struct process *p, const char *name, const uint8_t *img, uint64_t size,
             struct elf_image *im, int *elf_err);

#endif
