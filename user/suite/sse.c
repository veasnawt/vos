/* SSE with CR0.EM = 1: #UD. Also rejected by the build-time validator. */
#include "fixture.h"

int main(void)
{
    __asm__ volatile ("xorps %xmm0, %xmm0");
    not_reached();
}
