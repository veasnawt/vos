/*
 * Validation of VOS ABI v0 executables (static ELF64 x86_64, plan section 8.4).
 *
 * elf_validate is a pure function over an untrusted byte buffer: it allocates
 * nothing, reads only inside [img, img + size), never follows section headers
 * and uses overflow-free arithmetic (every 64-bit field it reads must be below
 * 2^40, the same rule as scripts/check-user-elf.sh). It is built into the
 * kernel and, unchanged, into a host tool that the tests compare with the
 * build-time validator.
 *
 * Trust boundary: the build-time checks that need section headers or
 * disassembly (no relocation, dynamic or TLS sections, no %fs/%gs operands,
 * no x87/MMX/SSE) cannot be made here. A file that skips them is still
 * confined to its own address space: x87/SSE trap, %fs/%gs have base 0, and
 * unapplied relocations only break the program itself.
 */
#ifndef VOS_X86_64_ELF_H
#define VOS_X86_64_ELF_H

#include <stdint.h>

#define ELF_MAX_FILE        (1ULL << 20)
#define ELF_MAX_LOADS       4
#define ELF_MAX_PHNUM       8
#define ELF_MAX_IMAGE_PAGES 256         /* 1 MiB */

#define ELF_PF_X    1u
#define ELF_PF_W    2u
#define ELF_PF_R    4u

/* Results: 0 = valid; each rule group has its own code (see elf_strerror). */
enum {
    ELF_OK = 0,
    ELF_ESIZE,          /* file smaller than a header or larger than 1 MiB */
    ELF_EMAGIC,
    ELF_EIDENT,         /* class, data, version, OS/ABI, ABI version, padding */
    ELF_EDYN,           /* ET_DYN: PIE or shared object */
    ELF_ETYPE,          /* other e_type */
    ELF_EMACHINE,
    ELF_EHEADER,        /* e_version, e_flags, e_ehsize, e_phentsize, e_phnum, e_phoff */
    ELF_EPHBOUNDS,      /* program-header table outside the file */
    ELF_ERANGE,         /* a 64-bit field >= 2^40 */
    ELF_ELOADCOUNT,     /* 0 or more than 4 PT_LOAD */
    ELF_EALIGN,         /* PT_LOAD p_align, p_vaddr, p_offset */
    ELF_ESEGSIZE,       /* p_memsz 0, p_filesz > p_memsz, file range outside the file */
    ELF_EUSERRANGE,     /* segment outside [0x400000, 0x40000000) */
    ELF_EFLAGS,         /* unknown flags or not readable */
    ELF_EWX,            /* writable and executable */
    ELF_EOVERLAP,       /* unsorted or overlapping pages */
    ELF_ENOEXEC,        /* no executable segment */
    ELF_EIMAGE,         /* more than 1 MiB of memory */
    ELF_EENTRY,         /* entry not in the file part of an executable segment */
    ELF_ENOTE,          /* PT_NOTE alignment, bounds or note structure */
    ELF_ENOTEMISSING,   /* no VOS ABI note */
    ELF_ENOTEDUP,       /* more than one VOS ABI note */
    ELF_EABI,           /* VOS ABI note descsz or version */
    ELF_ESTACK,         /* executable stack requested */
    ELF_EPTYPE,         /* forbidden or unknown program-header type */
    ELF_NERRORS
};

struct elf_segment {
    uint64_t vaddr, memsz;              /* page-aligned vaddr; memsz > 0 */
    uint64_t offset, filesz;            /* file bytes to copy; filesz <= memsz */
    uint32_t flags;                     /* ELF_PF_* */
};

struct elf_image {
    uint64_t entry;
    unsigned nload;
    struct elf_segment load[ELF_MAX_LOADS];     /* sorted by vaddr, disjoint pages */
    uint64_t pages;                     /* pages needed for all segments */
    uint32_t abi;                       /* VOS ABI version from the note: 0 or 1 (N2.4) */
};

#define ELF_ABI_MAX     1               /* the newest ABI version the loader accepts */

int elf_validate(const uint8_t *img, uint64_t size, struct elf_image *out);
const char *elf_strerror(int err);

#endif
