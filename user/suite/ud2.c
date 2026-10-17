/* Invalid opcode: #UD. */
#include "fixture.h"

int main(void)
{
    __asm__ volatile ("ud2");
    not_reached();
}
