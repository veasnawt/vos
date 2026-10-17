/* Privileged: #GP(0). */
#include "fixture.h"

int main(void)
{
    __asm__ volatile ("hlt");
    not_reached();
}
