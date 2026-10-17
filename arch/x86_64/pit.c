#include "pit.h"
#include "cpu.h"

_Static_assert(PIT_DIVISOR == 11932, "PIT divisor for 100 Hz");
_Static_assert(PIT_DIVISOR > 1 && PIT_DIVISOR <= 0xFFFF, "PIT divisor must fit 16 bits");

void pit_init(void)
{
    outb(PIT_CMD, PIT_CMD_CH0_MODE2);
    outb(PIT_CH0, PIT_DIVISOR & 0xFF);
    outb(PIT_CH0, PIT_DIVISOR >> 8);
}

void pit_readback(uint8_t *status, uint16_t *count)
{
    outb(PIT_CMD, PIT_READBACK_CH0);
    *status = inb(PIT_CH0);
    uint8_t lo = inb(PIT_CH0);
    *count = (uint16_t)(lo | inb(PIT_CH0) << 8);
}
