/*
 * 8253/8254 programmable interval timer, channel 0 (wired to IRQ0).
 *
 * The input clock is 1193182 Hz (exactly 105/88 MHz = 1193181.8 Hz). For
 * about 100 Hz the divisor is round(1193182 / 100) = 11932, giving
 * 99.998 Hz, one tick every 10.0002 ms. Channel 0 runs in mode 2 (rate
 * generator), binary, low byte then high byte.
 *
 * Under QEMU TCG the tick rate follows the host clock only loosely; nothing
 * in VOS may depend on ticks being a precise measure of time.
 */
#ifndef VOS_X86_64_PIT_H
#define VOS_X86_64_PIT_H

#include <stdint.h>

#define PIT_INPUT_HZ        1193182u
#define PIT_HZ              100u
#define PIT_DIVISOR         ((PIT_INPUT_HZ + PIT_HZ / 2) / PIT_HZ)     /* 11932 */
#define PIT_CH0             0x40
#define PIT_CMD             0x43
#define PIT_CMD_CH0_MODE2   0x34    /* channel 0, lobyte/hibyte, mode 2, binary */
#define PIT_READBACK_CH0    0xC2    /* read-back: latch count and status of channel 0 */
#define PIT_STATUS_CFG_MASK 0x3F    /* access mode, operating mode, BCD */

void pit_init(void);
/* Latch and read channel 0: its status byte and current count. */
void pit_readback(uint8_t *status, uint16_t *count);

#endif
