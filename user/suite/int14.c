/* int $14 hits a DPL 0 gate: #GP(14 * 8 + 2), no forged page fault. */
#include "fixture.h"

int main(void)
{
    __asm__ volatile ("int $14");
    not_reached();
}
