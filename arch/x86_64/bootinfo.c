#include "bootinfo.h"
#include "lib.h"
#include "memlayout.h"

#define LOW_MEM_END     0x100000ULL     /* physical 0-1 MiB is always reserved */
#define PAGE0_END       0x1000ULL       /* physical page 0: never a loader object */
#define LEGACY_HOLE     0xA0000ULL      /* [0xA0000, 1 MiB): video memory and ROMs, never RAM */

static uint64_t page_down(uint64_t x)
{
    return x & ~(PAGE_SIZE - 1);
}

static uint64_t page_up(uint64_t x)     /* callers keep x <= BOOT_MAP_LIMIT */
{
    return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

/* True if the tag's string area is non-empty and ends in NUL. */
static int terminated(const char *s, uint32_t len)
{
    return len > 0 && s[len - 1] == '\0';
}

const char *mb2_parse(const void *info, uint64_t info_phys, struct boot_info *bi)
{
    memset(bi, 0, sizeof(*bi));
    if (info_phys == 0 || info_phys % MB2_TAG_ALIGN != 0 ||
        info_phys > BOOT_MAP_LIMIT - sizeof(struct mb2_info))
        return "info block address invalid";

    const uint8_t *base = info;
    uint32_t total = ((const struct mb2_info *)base)->total_size;

    if (total < sizeof(struct mb2_info) + sizeof(struct mb2_tag) || total > MB2_INFO_MAX ||
        info_phys + total > BOOT_MAP_LIMIT)
        return "info block size invalid";
    bi->info_start = info_phys;
    bi->info_end = info_phys + total;

    uint32_t off = sizeof(struct mb2_info);
    while (off + sizeof(struct mb2_tag) <= total) {
        const struct mb2_tag *tag = (const void *)(base + off);

        if (tag->size < sizeof(*tag) || tag->size > total - off)
            return "tag size invalid";
        switch (tag->type) {
        case MB2_TAG_END:
            if (tag->size != sizeof(*tag))
                return "end tag size invalid";
            return bi->mmap ? NULL : "no memory map";
        case MB2_TAG_CMDLINE: {
            const struct mb2_tag_string *s = (const void *)tag;

            if (bi->cmdline)
                return "duplicate command line tag";
            if (!terminated(s->string, tag->size - sizeof(*tag)))
                return "command line not terminated";
            bi->cmdline = s->string;
            break;
        }
        case MB2_TAG_LOADER_NAME: {
            const struct mb2_tag_string *s = (const void *)tag;

            if (terminated(s->string, tag->size - sizeof(*tag)))
                bi->loader = s->string;
            break;
        }
        case MB2_TAG_MODULE: {
            const struct mb2_tag_module *m = (const void *)tag;

            if (tag->size < sizeof(*m))
                return "module tag too small";
            if (!terminated(m->cmdline, tag->size - sizeof(*m)))
                return "module command line not terminated";
            if (m->mod_start >= m->mod_end)
                return "module range empty or reversed";
            if (m->mod_end > BOOT_MAP_LIMIT)
                return "module extends above 1 GiB";
            if (bi->module_count >= BOOT_MAX_MODULES)
                return "too many modules";
            bi->modules[bi->module_count].start = m->mod_start;
            bi->modules[bi->module_count].end = m->mod_end;
            bi->modules[bi->module_count].cmdline = m->cmdline;
            bi->module_count++;
            break;
        }
        case MB2_TAG_MMAP: {
            const struct mb2_tag_mmap *m = (const void *)tag;

            if (bi->mmap)
                return "duplicate memory map tag";
            if (tag->size < sizeof(*m) || m->entry_size < sizeof(struct mb2_mmap_entry) ||
                m->entry_size % 8 != 0)
                return "memory map tag invalid";
            if (m->entry_version != 0)
                return "memory map version unsupported";
            if (tag->size == sizeof(*m))
                return "memory map empty";
            if ((tag->size - sizeof(*m)) % m->entry_size != 0)
                return "memory map has a partial entry";
            bi->mmap = m;
            bi->mmap_entries = (tag->size - sizeof(*m)) / m->entry_size;
            break;
        }
        }
        off += (tag->size + MB2_TAG_ALIGN - 1) & ~(uint32_t)(MB2_TAG_ALIGN - 1);
    }
    return "no end tag";
}

const struct mb2_mmap_entry *boot_mmap_entry(const struct boot_info *bi, uint32_t i)
{
    return (const void *)((const uint8_t *)bi->mmap + sizeof(*bi->mmap) +
                          (size_t)i * bi->mmap->entry_size);
}

/* [start, end) lies wholly inside one available entry (entries that overflow are ignored). */
static int in_usable_ram(const struct boot_info *bi, uint64_t start, uint64_t end)
{
    for (uint32_t i = 0; i < bi->mmap_entries; i++) {
        const struct mb2_mmap_entry *e = boot_mmap_entry(bi, i);

        if (e->type == MB2_MMAP_AVAILABLE && e->length != 0 &&
            e->base + e->length >= e->base &&
            start >= e->base && end <= e->base + e->length)
            return 1;
    }
    return 0;
}

/*
 * [start, end) intersects an entry that is NOT available (reserved, ACPI, bad,
 * unknown). A non-available entry whose end overflows is taken to extend to 2^64.
 */
static int hits_unavailable(const struct boot_info *bi, uint64_t start, uint64_t end)
{
    for (uint32_t i = 0; i < bi->mmap_entries; i++) {
        const struct mb2_mmap_entry *e = boot_mmap_entry(bi, i);
        uint64_t e_end = e->base + e->length;

        if (e->type == MB2_MMAP_AVAILABLE || e->length == 0)
            continue;
        if (e_end < e->base)
            e_end = UINT64_MAX;
        if (start < e_end && e->base < end)
            return 1;
    }
    return 0;
}

static int overlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1)
{
    return a0 < b1 && b0 < a1;
}

/* Add a range that must be real, conflict-free RAM below 1 GiB. */
static const char *reserve_ram(const struct boot_info *bi, struct resv_table *t,
                               uint64_t start, uint64_t end, const char *what,
                               const char *bad)
{
    if (start >= end || end > BOOT_MAP_LIMIT)
        return bad;
    if (!in_usable_ram(bi, page_down(start), page_up(end)) ||
        hits_unavailable(bi, page_down(start), page_up(end)))
        return bad;
    switch (resv_add(t, start, end, what)) {
    case RESV_OK:
        return NULL;
    case RESV_FULL:
        return "reservation table full";
    default:
        return bad;
    }
}

/*
 * U1: placement rules for a loader object (the info block or a module),
 * wherever the loader put it. GRUB on UEFI places them below 1 MiB (OVMF:
 * module 0x64000, info 0x68000), on BIOS above the kernel. An object
 *   - is non-empty and ends at or below 1 GiB (parse checks; repeated here);
 *   - never touches physical page 0 (the null page; BIOS IVT/BDA);
 *   - never touches the legacy hole [0xA0000, 1 MiB), even if a map calls it RAM;
 *   - shares no page with the kernel image (kernel pages carry W^X permissions);
 *   - then must pass reserve_ram: whole pages inside one available entry,
 *     touching no non-available entry (EBDA, ACPI, reserved, firmware).
 * Byte overlaps between loader objects are refused by the caller; two
 * objects may share a page (the reservation table is a union of pages).
 */
static const char *place_object(const struct boot_info *bi, struct resv_table *t, uint64_t kstart,
                                uint64_t kend, uint64_t start, uint64_t end, const char *what,
                                const char *page0, const char *hole, const char *kernel, const char *bad)
{
    if (start >= end || end > BOOT_MAP_LIMIT)
        return bad;
    if (page_down(start) < PAGE0_END)
        return page0;
    if (overlap(page_down(start), page_up(end), LEGACY_HOLE, LOW_MEM_END))
        return hole;
    if (overlap(page_down(start), page_up(end), page_down(kstart), page_up(kend)))
        return kernel;
    return reserve_ram(bi, t, start, end, what, bad);
}

const char *boot_reserve(const struct boot_info *bi, uint64_t kstart, uint64_t kend,
                         struct resv_table *t)
{
    const char *err;

    if (!bi->mmap)
        return "no memory map";
    if (resv_add(t, 0, LOW_MEM_END, "low 1 MiB") != RESV_OK)
        return "reservation table full";
    if (kstart < LOW_MEM_END)
        return "kernel image below 1 MiB";
    if ((err = reserve_ram(bi, t, kstart, kend, "kernel image", "kernel image not in usable RAM")))
        return err;
    if ((err = place_object(bi, t, kstart, kend, bi->info_start, bi->info_end, "multiboot2 info",
                            "multiboot2 info in physical page 0",
                            "multiboot2 info in the legacy hole (0xA0000-1 MiB)",
                            "multiboot2 info shares a page with the kernel image",
                            "multiboot2 info not in usable RAM")))
        return err;

    for (uint32_t i = 0; i < bi->module_count; i++) {
        const struct boot_module *m = &bi->modules[i];

        if (overlap(m->start, m->end, bi->info_start, bi->info_end))
            return "module overlaps the multiboot2 info";
        for (uint32_t j = 0; j < i; j++)
            if (overlap(m->start, m->end, bi->modules[j].start, bi->modules[j].end))
                return "modules overlap";
        if ((err = place_object(bi, t, kstart, kend, m->start, m->end, "module", "module in physical page 0",
                                "module in the legacy hole (0xA0000-1 MiB)", "module overlaps the kernel image",
                                "module not in usable RAM")))
            return err;
    }
    return NULL;
}
