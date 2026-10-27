/* Optimized hosted validator comparison only; never mix with sanitizer timing. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "elf.h"
int elf_validate_reference(const uint8_t *,uint64_t,struct elf_image *);
static volatile uint64_t sink;
__attribute__((noreturn)) void panic(const char *message)
{fprintf(stderr,"unexpected Rust panic: %s\n",message);exit(2);}
static uint64_t now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) exit(2);
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static uint64_t measure(int (*validate)(const uint8_t *,uint64_t,struct elf_image *),
                        const uint8_t *bytes,uint64_t size,unsigned count)
{
    uint64_t start=now();struct elf_image image;
    for (unsigned i=0;i<count;i++) {
        if (validate(bytes,size,&image)!=ELF_OK) exit(1);
        sink+=image.entry;
    }
    return now()-start;
}
int main(int argc,char **argv)
{
    if (argc!=2) return 2;
    FILE *f=fopen(argv[1],"rb");if (!f || fseek(f,0,SEEK_END)) return 2;
    long n=ftell(f);if (n<64 || (unsigned long)n>ELF_MAX_FILE || fseek(f,0,SEEK_SET)) return 2;
    uint8_t *bytes=malloc((size_t)n);
    if (!bytes || fread(bytes,1,(size_t)n,f)!=(size_t)n || fclose(f)) return 2;
    (void)measure(elf_validate,bytes,n,1000);
    (void)measure(elf_validate_reference,bytes,n,1000);
    puts("pair,validations,C_reference_ns,Rust_backed_ns");
    for (unsigned i=0;i<5;i++) {
        uint64_t c,r;
        if (i%2) {r=measure(elf_validate,bytes,n,200000);c=measure(elf_validate_reference,bytes,n,200000);}
        else {c=measure(elf_validate_reference,bytes,n,200000);r=measure(elf_validate,bytes,n,200000);}
        printf("%u,200000,%llu,%llu\n",i,(unsigned long long)c,(unsigned long long)r);
    }
    free(bytes);return 0;
}
