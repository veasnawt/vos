/* VOS early-boot font (U1): original 8x16 glyphs for printable ASCII (scripts/font8x16.py). */
#ifndef VOS_X86_64_FONT8X16_H
#define VOS_X86_64_FONT8X16_H

#include <stdint.h>

#define FONT8X16_FIRST  0x20
#define FONT8X16_GLYPHS 95      /* 0x20-0x7E */

extern const uint8_t font8x16[FONT8X16_GLYPHS][16];

#endif
