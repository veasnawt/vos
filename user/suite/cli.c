/* Privileged at CPL 3 with IOPL 0: #GP(0). */
#include "fixture.h"

int main(void)
{
    __asm__ volatile ("cli");
    not_reached();
}
