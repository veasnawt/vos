/* Write to the program's own code (R+X): #PF P=1 W=1 U=1 at 0x400000. */
#include "fixture.h"

int main(void)
{
    *(volatile unsigned char *)0x400000 = 0x90;
    not_reached();
}
