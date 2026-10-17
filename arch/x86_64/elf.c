/*
 * VOS ABI v0 executable validation. Freestanding and dependency-free so the
 * same file builds into the kernel and into the host tool used by the tests.
 * Rules and their order follow scripts/check-user-elf.sh (part 1).
 */
#include "elf.h"

#include <stddef.h>

#define PAGE            0x1000ULL
#define USER_START      0x400000ULL     /* memlayout.h USER_IMAGE_START */
#define USER_END        0x40000000ULL   /* memlayout.h USER_IMAGE_END */
#define FIELD_LIMIT     (1ULL << 40)    /* every 64-bit field read must be below this */

#define ET_EXEC         2
#define ET_DYN          3
#define EM_X86_64       62
#define PT_LOAD         1
#define PT_NOTE         4
#define PT_GNU_STACK    0x6474e551u
#define VOS_NOTE_ABI    1

/* Little-endian read of n bytes at off; the caller has checked the bounds. */
static uint64_t le(const uint8_t *img, uint64_t off, unsigned n)
{
    uint64_t v = 0;

    while (n--)
        v = v << 8 | img[off + n];
    return v;
}

static int inside(uint64_t size, uint64_t off, uint64_t len)
{
    return off <= size && len <= size - off;
}

int elf_validate(const uint8_t *img, uint64_t size, struct elf_image *out)
{
    struct elf_image r = { 0 };
    unsigned exec_loads = 0, vos_notes = 0, entry_ok = 0;
    uint64_t prev_end = 0;

    *out = r;
    if (size < 64 || size > ELF_MAX_FILE)
        return ELF_ESIZE;
    if (img[0] != 0x7F || img[1] != 'E' || img[2] != 'L' || img[3] != 'F')
        return ELF_EMAGIC;
    if (img[4] != 2 || img[5] != 1 || img[6] != 1 || img[7] != 0 || img[8] != 0)
        return ELF_EIDENT;
    for (int i = 9; i < 16; i++)
        if (img[i])
            return ELF_EIDENT;

    uint64_t type = le(img, 16, 2);
    if (type == ET_DYN)
        return ELF_EDYN;
    if (type != ET_EXEC)
        return ELF_ETYPE;
    if (le(img, 18, 2) != EM_X86_64)
        return ELF_EMACHINE;
    if (le(img, 20, 4) != 1)
        return ELF_EHEADER;
    uint64_t entry = le(img, 24, 8), phoff = le(img, 32, 8);
    if (entry >= FIELD_LIMIT || phoff >= FIELD_LIMIT)
        return ELF_ERANGE;
    if (le(img, 48, 4) != 0 || le(img, 52, 2) != 64 || le(img, 54, 2) != 56)
        return ELF_EHEADER;
    uint64_t phnum = le(img, 56, 2);
    if (phnum < 1 || phnum > ELF_MAX_PHNUM || phoff < 64 || phoff % 8 != 0)
        return ELF_EHEADER;
    if (!inside(size, phoff, phnum * 56))
        return ELF_EPHBOUNDS;

    for (uint64_t i = 0; i < phnum; i++) {
        uint64_t ph = phoff + i * 56;
        uint32_t ptype = (uint32_t)le(img, ph, 4), flags = (uint32_t)le(img, ph + 4, 4);
        uint64_t off = le(img, ph + 8, 8), vaddr = le(img, ph + 16, 8);
        uint64_t filesz = le(img, ph + 32, 8), memsz = le(img, ph + 40, 8);
        uint64_t align = le(img, ph + 48, 8);

        if (off >= FIELD_LIMIT || vaddr >= FIELD_LIMIT || filesz >= FIELD_LIMIT ||
            memsz >= FIELD_LIMIT || align >= FIELD_LIMIT)
            return ELF_ERANGE;
        /* From here on every sum of two fields is below 2^41: no overflow. */

        switch (ptype) {
        case PT_LOAD: {
            if (r.nload == ELF_MAX_LOADS)
                return ELF_ELOADCOUNT;
            if (align != PAGE || vaddr % PAGE != 0 || off % PAGE != 0)
                return ELF_EALIGN;
            if (memsz == 0 || filesz > memsz || off + filesz > size)
                return ELF_ESEGSIZE;
            if (vaddr < USER_START || vaddr + memsz > USER_END)
                return ELF_EUSERRANGE;
            if (flags & ~(ELF_PF_R | ELF_PF_W | ELF_PF_X) || !(flags & ELF_PF_R))
                return ELF_EFLAGS;
            if ((flags & ELF_PF_W) && (flags & ELF_PF_X))
                return ELF_EWX;
            if (vaddr < prev_end)
                return ELF_EOVERLAP;
            prev_end = (vaddr + memsz + PAGE - 1) & ~(PAGE - 1);
            r.pages += (memsz + PAGE - 1) / PAGE;
            if (flags & ELF_PF_X) {
                exec_loads++;
                if (entry >= vaddr && entry < vaddr + filesz)
                    entry_ok = 1;
            }
            r.load[r.nload++] = (struct elf_segment){ vaddr, memsz, off, filesz, flags };
            break;
        }
        case PT_NOTE: {
            if (align != 4 || off + filesz > size)
                return ELF_ENOTE;
            uint64_t n = off, end = off + filesz;
            while (end - n >= 12) {
                uint64_t namesz = le(img, n, 4), descsz = le(img, n + 4, 4);
                uint64_t ntype = le(img, n + 8, 4);
                uint64_t na = (namesz + 3) & ~3ULL, da = (descsz + 3) & ~3ULL;

                if (na + da > end - n - 12)
                    return ELF_ENOTE;
                if (namesz == 4 && img[n + 12] == 'V' && img[n + 13] == 'O' &&
                    img[n + 14] == 'S' && img[n + 15] == 0 && ntype == VOS_NOTE_ABI) {
                    if (descsz != 4 || le(img, n + 16, 4) > ELF_ABI_MAX)
                        return ELF_EABI;
                    r.abi = (uint32_t)le(img, n + 16, 4);
                    if (++vos_notes > 1)
                        return ELF_ENOTEDUP;
                }
                n += 12 + na + da;
            }
            if (n != end)
                return ELF_ENOTE;
            break;
        }
        case PT_GNU_STACK:
            if (flags & ELF_PF_X)
                return ELF_ESTACK;
            break;
        default:                        /* PT_NULL, INTERP, DYNAMIC, TLS, PHDR, ... */
            return ELF_EPTYPE;
        }
    }
    if (r.nload == 0)
        return ELF_ELOADCOUNT;
    if (exec_loads == 0)
        return ELF_ENOEXEC;
    if (r.pages > ELF_MAX_IMAGE_PAGES)
        return ELF_EIMAGE;
    if (!entry_ok)
        return ELF_EENTRY;
    if (vos_notes != 1)
        return ELF_ENOTEMISSING;
    r.entry = entry;
    *out = r;
    return ELF_OK;
}

const char *elf_strerror(int err)
{
    static const char *const names[ELF_NERRORS] = {
        [ELF_OK] = "valid",
        [ELF_ESIZE] = "file smaller than an ELF header or larger than 1 MiB",
        [ELF_EMAGIC] = "bad ELF magic",
        [ELF_EIDENT] = "unsupported e_ident (class, data, version, OS/ABI or padding)",
        [ELF_EDYN] = "ET_DYN (PIE or shared object); VOS requires ET_EXEC",
        [ELF_ETYPE] = "e_type is not ET_EXEC",
        [ELF_EMACHINE] = "e_machine is not EM_X86_64",
        [ELF_EHEADER] = "unsupported ELF header field",
        [ELF_EPHBOUNDS] = "program-header table outside the file",
        [ELF_ERANGE] = "64-bit field out of range",
        [ELF_ELOADCOUNT] = "not 1-4 PT_LOAD segments",
        [ELF_EALIGN] = "PT_LOAD not page-aligned",
        [ELF_ESEGSIZE] = "PT_LOAD sizes or file range invalid",
        [ELF_EUSERRANGE] = "PT_LOAD outside [0x400000, 0x40000000)",
        [ELF_EFLAGS] = "PT_LOAD flags unknown or not readable",
        [ELF_EWX] = "PT_LOAD writable and executable",
        [ELF_EOVERLAP] = "PT_LOAD unsorted or overlapping",
        [ELF_ENOEXEC] = "no executable PT_LOAD",
        [ELF_EIMAGE] = "image needs more than 1 MiB",
        [ELF_EENTRY] = "entry not in the file-backed part of an executable PT_LOAD",
        [ELF_ENOTE] = "malformed PT_NOTE",
        [ELF_ENOTEMISSING] = "no VOS ABI note",
        [ELF_ENOTEDUP] = "more than one VOS ABI note",
        [ELF_EABI] = "VOS ABI note: bad descriptor or unsupported version",
        [ELF_ESTACK] = "executable stack requested",
        [ELF_EPTYPE] = "unsupported program-header type",
    };

    return err >= 0 && err < ELF_NERRORS ? names[err] : "?";
}
