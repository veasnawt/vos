/* int $0x81 hits an absent DPL 0 gate: #GP(0x81 * 8 + 2). */
#include "fixture.h"

int main(void)
{
    __asm__ volatile ("int $0x81");
    not_reached();
}
