/* Compare the real Rust-backed validator with its hosted C reference. */
#include <stdio.h>
#include <stdlib.h>
#include "elf.h"

int elf_validate_reference(const uint8_t *, uint64_t, struct elf_image *);

static int equal(const struct elf_image *a, const struct elf_image *b)
{
    if (a->entry!=b->entry || a->pages!=b->pages || a->abi!=b->abi || a->nload!=b->nload)
        return 0;
    for (unsigned i=0;i<ELF_MAX_LOADS;i++) {
        const struct elf_segment *x=&a->load[i], *y=&b->load[i];
        if (x->vaddr!=y->vaddr || x->memsz!=y->memsz || x->offset!=y->offset ||
            x->filesz!=y->filesz || x->flags!=y->flags) return 0;
    }
    return 1; /* Compare fields, never unspecified structure padding. */
}

__attribute__((noreturn)) void panic(const char *message)
{
    fprintf(stderr,"unexpected Rust panic: %s\n",message);exit(2);
}

int main(int argc, char **argv)
{
    unsigned accepted=0,rejected=0;
    for (int i=1;i<argc;i++) {
        FILE *f=fopen(argv[i],"rb");
        if (!f || fseek(f,0,SEEK_END)) return 2;
        long n=ftell(f);
        if (n<0 || (unsigned long)n>ELF_MAX_FILE+1 || fseek(f,0,SEEK_SET)) return 2;
        uint8_t *bytes=malloc(n ? (size_t)n : 1);
        if (!bytes || fread(bytes,1,(size_t)n,f)!=(size_t)n || fclose(f)) return 2;
        struct elf_image a,b;
        int x=elf_validate(bytes,(uint64_t)n,&a);
        int y=elf_validate_reference(bytes,(uint64_t)n,&b);
        free(bytes);
        if (x!=y || !equal(&a,&b)) {
            fprintf(stderr,"validator mismatch: %s Rust=%d C=%d\n",argv[i],x,y);return 1;
        }
        if (x==ELF_OK) accepted++;else rejected++;
    }
    printf("R1 validator differential: %u files PASS (%u accepted, %u rejected); codes and all image fields agree\n",
           accepted+rejected,accepted,rejected);
    return accepted && rejected ? 0 : 1;
}
