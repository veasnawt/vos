/* Write VGA text memory at its physical address: not mapped for users (#PF W=1 U=1). */
#include "fixture.h"

int main(void)
{
    *(volatile unsigned short *)0xB8000UL = 0x4F21;
    not_reached();
}
