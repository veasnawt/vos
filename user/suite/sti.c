/* N2.1: sti at CPL 3 with IOPL 0 raises #GP(0). */
#include "fixture.h"

int main(void)
{
    __asm__ volatile ("sti");
    not_reached();
}
