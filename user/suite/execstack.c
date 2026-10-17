/* Execute from the stack (RW, NX): #PF P=1 U=1 I/D=1, RIP = CR2. */
#include "fixture.h"

int main(void)
{
    volatile unsigned char code[16];

    code[0] = 0xC3;                     /* ret */
    ((void (*)(void))(unsigned long)code)();
    not_reached();
}
