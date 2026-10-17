/* Unbounded recursion: the stack runs into the unmapped guard page (#PF W=1 U=1). */
#include "fixture.h"

static volatile int limit = 1 << 30;

static int deep(int n)
{
    volatile char pad[512];

    pad[0] = (char)n;
    return n < limit ? deep(n + 1) + pad[0] : 0;
}

int main(void)
{
    deep(0);
    not_reached();
}
