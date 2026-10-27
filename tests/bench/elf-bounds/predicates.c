/* Benchmark-only functions. Never linked into the VOS kernel. */
#include <stdint.h>

__attribute__((noinline,noclone))
int bench_c_inside(uint64_t size,uint64_t off,uint64_t len)
{
    return off <= size && len <= size - off;
}

/* Loop + indirect-call + minimal integer work control, not a pure call timer. */
__attribute__((noinline,noclone))
int bench_call_control(uint64_t size,uint64_t off,uint64_t len)
{
    return (int)((size ^ off ^ len) & 1);
}
