/*
 * Physical reservation table: frames that must never be handed out as free.
 *
 * Format: each entry is a half-open physical range [start, end), rounded
 * OUTWARD to whole 4 KiB pages when it is added. Entries may overlap (for
 * example the Multiboot2 info block lies inside the kernel image's page span);
 * the table means the UNION of its entries. An entry is never dropped: when
 * the table is full, resv_add fails and leaves the table unchanged.
 */
#ifndef VOS_X86_64_RESV_H
#define VOS_X86_64_RESV_H

#include <stddef.h>
#include <stdint.h>

#define RESV_MAX        16

struct phys_range {
    uint64_t start, end;                /* [start, end), page-aligned */
    const char *what;
};

struct resv_table {
    struct phys_range r[RESV_MAX];
    size_t count;
};

#define RESV_OK         0
#define RESV_BAD_RANGE  1               /* empty, reversed, or rounding would overflow */
#define RESV_FULL       2

void resv_init(struct resv_table *t);
int resv_add(struct resv_table *t, uint64_t start, uint64_t end, const char *what);
/* Nonzero if any reserved page intersects [start, end). */
int resv_overlaps(const struct resv_table *t, uint64_t start, uint64_t end);

#endif
