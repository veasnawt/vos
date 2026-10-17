#include "lib.h"

#include <stdint.h>

/* rep movs/stos instead of loops: gcc could turn a loop back into a call to itself. */
void *memset(void *dst, int c, size_t n)
{
    void *d = dst;

    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    void *d = dst;

    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;

    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}
