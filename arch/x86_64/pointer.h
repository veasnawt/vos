/* Standard three-byte PS/2 auxiliary packets; no wheel/USB/hotplug. */
#ifndef VOS_X86_64_POINTER_H
#define VOS_X86_64_POINTER_H
#include <stdint.h>
struct pointer_packet {int16_t dx,dy;uint8_t buttons;};
struct pointer_decoder {uint8_t bytes[3],used,idle;};
int pointer_decode(struct pointer_decoder *,uint8_t,struct pointer_packet *);
int pointer_idle(struct pointer_decoder *);
#endif
