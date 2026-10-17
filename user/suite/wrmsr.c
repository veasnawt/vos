/* MSR write: #GP(0). */
#include "fixture.h"

int main(void)
{
    __asm__ volatile ("wrmsr" : : "c"(0xC0000080u), "a"(0u), "d"(0u));
    not_reached();
}
