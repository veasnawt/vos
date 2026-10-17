/* Freestanding string and memory routines (the kernel is built -fno-builtin). */
#ifndef VOS_X86_64_LIB_H
#define VOS_X86_64_LIB_H

#include <stddef.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);    /* regions must not overlap */
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
