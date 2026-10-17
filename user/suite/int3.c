/* int3 hits the DPL 0 breakpoint gate: #GP(3 * 8 + 2). */
#include "fixture.h"

int main(void)
{
    __asm__ volatile ("int3");
    not_reached();
}
