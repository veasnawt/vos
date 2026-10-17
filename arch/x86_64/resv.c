#include "resv.h"
#include "memlayout.h"

void resv_init(struct resv_table *t)
{
    t->count = 0;
}

int resv_add(struct resv_table *t, uint64_t start, uint64_t end, const char *what)
{
    if (start >= end || end > UINT64_MAX - (PAGE_SIZE - 1))
        return RESV_BAD_RANGE;
    if (t->count >= RESV_MAX)
        return RESV_FULL;

    struct phys_range *r = &t->r[t->count++];
    r->start = start & ~(PAGE_SIZE - 1);
    r->end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    r->what = what;
    return RESV_OK;
}

int resv_overlaps(const struct resv_table *t, uint64_t start, uint64_t end)
{
    if (start >= end)
        return 0;
    for (size_t i = 0; i < t->count; i++)
        if (start < t->r[i].end && t->r[i].start < end)
            return 1;
    return 0;
}
