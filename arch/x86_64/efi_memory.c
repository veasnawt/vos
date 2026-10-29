#include "efi_memory.h"

#include "multiboot2.h"
#include "pmm.h"

_Static_assert(sizeof(struct efi_memory_input) == 56, "efi_memory_input layout");
_Static_assert(sizeof(struct efi_memory_report) == 32, "efi_memory_report layout");

const char efi_memory_what[] = "firmware memory (EFI)";

static int add(void *ctx, uint64_t start, uint64_t end)
{
    return resv_add(ctx, start, end, EFI_MEMORY_WHAT) != RESV_OK;
}

static struct efi_memory_input input(const struct boot_info *bi, const struct resv_table *t)
{
    return (struct efi_memory_input){
        .mmap = bi->mmap,
        .efi_mmap = bi->efi_mmap,
        .window_lo = PMM_LOW_LIMIT,
        .window_hi = PMM_LIMIT,
        .mmap_size = bi->mmap ? bi->mmap->tag.size : 0,
        .efi_mmap_size = bi->efi_mmap ? bi->efi_mmap->size : 0,
        .efi_mmap_tags = bi->efi_mmap_tags,
        .efi_firmware = bi->efi != 0,
        .boot_services_active = bi->efi_bs_active != 0,
        .capacity = t->count < RESV_MAX ? (uint32_t)(RESV_MAX - t->count) : 0,
    };
}

int efi_memory_exclude(const struct boot_info *bi, struct resv_table *t,
                       struct efi_memory_report *report)
{
    struct efi_memory_input in = input(bi, t);

    return vos_efi_exclusions_v1(&in, add, t, report);
}

int efi_memory_firmware_frame(const struct boot_info *bi, const struct resv_table *t, uint64_t frame)
{
    int listed = 0;

    for (size_t i = 0; bi && t && i < t->count; i++)
        listed |= t->r[i].what == efi_memory_what && t->r[i].start <= frame &&
                  frame + PAGE_SIZE <= t->r[i].end;
    if (!listed)
        return 0;
    struct efi_memory_input in = input(bi, t);
    return vos_efi_firmware_frame_v1(&in, frame) == 1;
}

const char *efi_memory_strerror(uint32_t error)
{
    static const char *const why[] = {
        "unknown",
        "UEFI without an EFI memory map",
        "EFI boot services still active",
        "duplicate EFI memory map",
        "EFI map truncated",
        "bad EFI descriptor size",
        "bad EFI descriptor version",
        "bad EFI descriptor count",
        "unaligned EFI descriptor",
        "EFI descriptor overflows",
        "bad memory map tag",
        "bad window",
        "no reservation capacity",
        "reservation refused",
    };
    return error < sizeof(why) / sizeof(why[0]) ? why[error] : why[0];
}
