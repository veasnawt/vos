/* Same bare-metal Rust archive as the kernel; hosted C ABI/oracle checks only. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "rust_kernel.h"
__attribute__((noreturn)) void panic(const char *message)
{
    fprintf(stderr,"unexpected Rust panic in host ABI test: %s\n",message);
    exit(2);
}
int main(void)
{
    static const uint64_t edge[] = {0,1,UINT64_MAX,UINT64_C(1)<<63,
        UINT64_C(0x0123456789abcdef),UINT64_C(0xfedcba9876543210)};
    unsigned cases=0;
    for(unsigned i=0;i<sizeof(edge)/sizeof(edge[0]);i++)
        for(unsigned j=0;j<sizeof(edge)/sizeof(edge[0]);j++) {
            if(vos_rust_probe(edge[i],edge[j])!=rust_probe_expected(edge[i],edge[j]))return 1;
            cases++;
        }
    uint64_t seed=1,salt=UINT64_MAX;
    for(unsigned i=0;i<4096;i++) {
        seed=seed*UINT64_C(6364136223846793005)+1;
        salt=salt*UINT64_C(2862933555777941757)+UINT64_C(3037000493);
        if(vos_rust_probe(seed,salt)!=rust_probe_expected(seed,salt))return 1;
        cases++;
    }
    /* Independent fixed vectors, not just the C implementation of the formula. */
    if(vos_rust_probe(0,0)!=UINT64_C(0x564f535255535430) ||
       vos_rust_probe(UINT64_MAX,UINT64_MAX)!=UINT64_C(0xa9b0acadaaacabcE))return 1;
    printf("Rust kernel C ABI: %u pairs + 2 fixed vectors PASS; same archive, no host Rust runtime\n",cases);
    return 0;
}
