/*
 * Host build of the kernel's ELF validator (arch/x86_64/elf.c, unchanged).
 * usage: elfcheck FILE
 * Exit 0: accepted; 1: rejected (reason printed); 2: usage or I/O error.
 * The tests compare its verdicts with scripts/check-user-elf.sh.
 */
#include <stdio.h>
#include <stdlib.h>

#include "elf.h"

int main(int argc, char **argv)
{
    FILE *f;
    long size;
    unsigned char *buf;
    struct elf_image im;

    if (argc != 2 || !(f = fopen(argv[1], "rb"))) {
        fprintf(stderr, "usage: elfcheck FILE\n");
        return 2;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
        !(buf = malloc(size ? (size_t)size : 1)) || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "elfcheck: %s: read error\n", argv[1]);
        return 2;
    }
    fclose(f);
    int err = elf_validate(buf, (uint64_t)size, &im);
    if (err != ELF_OK) {
        printf("elfcheck: %s: REJECT: %s\n", argv[1], elf_strerror(err));
        return 1;
    }
    printf("elfcheck: %s: OK (%u PT_LOAD, entry 0x%llx, %llu pages)\n", argv[1], im.nload,
           (unsigned long long)im.entry, (unsigned long long)im.pages);
    return 0;
}
