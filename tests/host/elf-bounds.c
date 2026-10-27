/* R1: actual bare-metal Rust archive, original C and independent wide oracle. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "rust_kernel.h"

_Static_assert(sizeof(int) == 4, "Rust i32 / C int ABI");
__attribute__((noreturn)) void panic(const char *message)
{
    fprintf(stderr, "unexpected Rust panic: %s\n", message);
    exit(2);
}

static unsigned cases;
static void check(uint64_t size, uint64_t off, uint64_t len)
{
    int original = off <= size && len <= size - off;
    int wide = (__uint128_t)off + len <= (__uint128_t)size;
    int actual = vos_rust_elf_inside(size, off, len);
    if ((actual != 0 && actual != 1) || actual != original || actual != wide) {
        fprintf(stderr, "bounds mismatch: size=%" PRIu64 " off=%" PRIu64
                " len=%" PRIu64 " Rust=%d C=%d wide=%d\n",
                size, off, len, actual, original, wide);
        exit(1);
    }
    cases++;
}

int main(void)
{
    const uint64_t edge[] = {0,1,2,7,8,63,64,65,4095,4096,
                            UINT32_MAX,UINT64_C(1)<<32,UINT64_MAX-1,UINT64_MAX};
    for (unsigned s=0;s<sizeof(edge)/sizeof(edge[0]);s++)
        for (unsigned o=0;o<sizeof(edge)/sizeof(edge[0]);o++)
            for (unsigned n=0;n<sizeof(edge)/sizeof(edge[0]);n++)
                check(edge[s],edge[o],edge[n]);
    for (uint64_t s=0;s<=32;s++)
        for (uint64_t o=0;o<=32;o++)
            for (uint64_t n=0;n<=32;n++) check(s,o,n);
    uint64_t state=UINT64_C(0x564f535231);
    for (unsigned i=0;i<65536;i++) {
        state=state*UINT64_C(6364136223846793005)+1;uint64_t size=state;
        state=state*UINT64_C(6364136223846793005)+1;uint64_t off=state;
        state=state*UINT64_C(6364136223846793005)+1;uint64_t len=state;
        check(size,off,len);
        check(size,size,0);check(size,size,1);
        check(size,0,size);
        if (size<UINT64_MAX) {check(size,size+1,0);check(size,0,size+1);}
        if (off<=size) {
            check(size,off,size-off);
            if (size-off<UINT64_MAX) check(size,off,size-off+1);
        }
    }
    printf("R1 Rust bounds: %u cases PASS; original C and u128 oracles; exact 0/1\n",cases);
    return 0;
}
