/* B1: replay of a snapshot taken from a real QEMU boot (scripts/test-boot-resv-native.py) through the FROZEN C references.
 * usage: boot-resv-replay BOOT_INFO.bin MMAP_TAG.bin EFI_TAG.bin|- KSTART KEND   (KSTART, KEND in hex)
 * The snapshot is the struct boot_info and the memory-map and EFI tags exactly as the guest had them when boot_reserve was
 * called. This runs the accepted C boot_reserve and the accepted C efi_memory_exclude glue (resv_init, resv_add) on them and
 * prints the outcome in the form the native script derives from the guest's own results, so the two can be compared line by line.
 * Pointers into guest memory that the reservation code never reads (loader, cmdline, modules' command lines) stay as they were. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bootinfo.h"
#include "efi_memory.h"
#include "resv.h"

const char *boot_reserve_reference(const struct boot_info *, uint64_t, uint64_t, struct resv_table *);
void resv_init_reference(struct resv_table *);
int efi_memory_exclude_reference(const struct boot_info *, struct resv_table *, struct efi_memory_report *);
extern const char efi_memory_what_reference[];

__attribute__((noreturn)) void panic(const char *message)
{
    fprintf(stderr, "unexpected Rust panic: %s\n", message);
    exit(2);
}

static void *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    if (fseek(f, 0, SEEK_END) != 0)
        exit(2);
    long len = ftell(f);
    /* Only bounded snapshots from the observer are accepted. */
    if (len < 0 || len > (1 << 20) || fseek(f, 0, SEEK_SET) != 0)
        exit(2);
    void *p = malloc(len ? (size_t)len : 1);
    if (!p || fread(p, 1, (size_t)len, f) != (size_t)len)
        exit(2);
    if (fclose(f) != 0)
        exit(2);
    *n = (size_t)len;
    return p;
}

static void print_table(const struct resv_table *t)
{
    printf("table: %zu\n", t->count);
    for (size_t i = 0; i < t->count && i < RESV_MAX; i++)
        printf("e: 0x%llx 0x%llx %s\n", (unsigned long long)t->r[i].start, (unsigned long long)t->r[i].end,
               t->r[i].what == efi_memory_what_reference ? "<EFI>" : t->r[i].what);
}

int main(int argc, char **argv)
{
    static struct boot_info bi;
    static struct resv_table t;
    size_t n;

    if (argc != 6)
        return 2;
    void *b = slurp(argv[1], &n);
    if (n != sizeof bi)
        return 2;
    memcpy(&bi, b, n);
    free(b);
    if (bi.module_count > BOOT_MAX_MODULES)
        return 2;
    bi.mmap = slurp(argv[2], &n);
    if (n < sizeof *bi.mmap || bi.mmap->tag.size != n ||
        bi.mmap->entry_size < sizeof(struct mb2_mmap_entry) ||
        bi.mmap_entries > (n - sizeof *bi.mmap) / bi.mmap->entry_size)
        return 2;
    if (strcmp(argv[3], "-")) {
        bi.efi_mmap = slurp(argv[3], &n);
        if (n < sizeof *bi.efi_mmap || bi.efi_mmap->size != n)
            return 2;
    } else {
        bi.efi_mmap = NULL;
    }
    char *end;
    errno = 0;
    uint64_t ks = strtoull(argv[4], &end, 16);
    if (errno || end == argv[4] || *end || argv[4][0] == '-')
        return 2;
    errno = 0;
    uint64_t ke = strtoull(argv[5], &end, 16);
    if (errno || end == argv[5] || *end || argv[5][0] == '-')
        return 2;
    resv_init_reference(&t);
    const char *r = boot_reserve_reference(&bi, ks, ke, &t);
    printf("reserve: %s\n", r ? r : "(null)");
    print_table(&t);
    if (!r) {
        struct efi_memory_report rep;
        memset(&rep, 0, sizeof rep);
        int st = efi_memory_exclude_reference(&bi, &t, &rep);
        printf("efi: %d frames %llu error %u descriptors %u ranges %u coalesced %u\n", st, (unsigned long long)rep.frames, rep.error,
               rep.descriptors, rep.ranges, rep.coalesced);
        print_table(&t);
    }
    free((void *)bi.mmap);
    free((void *)bi.efi_mmap);
    return 0;
}
