/* x87 with CR0.EM = 1: #NM. Also rejected by the build-time validator. */
#include "fixture.h"

int main(void)
{
    __asm__ volatile ("fld1");
    not_reached();
}
