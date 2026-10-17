/* Execute from .data (RW, NX): #PF P=1 U=1 I/D=1. */
#include "fixture.h"

static unsigned char code[16] = { 0xC3 };

int main(void)
{
    ((void (*)(void))(unsigned long)code)();
    not_reached();
}
