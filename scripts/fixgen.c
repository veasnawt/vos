/*
 * fixgen: write deterministic file contents for the N3.3 ext2 fixtures
 * (host tool, built by scripts/mkext2fixtures.sh).
 *
 * usage: fixgen OUTPUT SIZE SEED
 *
 * Writes SIZE bytes of a xorshift64* stream seeded from the string SEED,
 * with the 8-byte little-endian file offset folded into every 8-byte word,
 * so that two files never share contents and a misplaced block is visible.
 * The output is a pure function of SIZE and SEED.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    unsigned long long size;
    char *end;
    uint64_t x = 0xCBF29CE484222325ULL;
    FILE *f;

    if (argc != 4) {
        fprintf(stderr, "usage: fixgen OUTPUT SIZE SEED\n");
        return 2;
    }
    errno = 0;
    size = strtoull(argv[2], &end, 10);
    if (errno || *end || size > (1ULL << 32)) {
        fprintf(stderr, "fixgen: SIZE must be 0..4294967296\n");
        return 2;
    }
    for (const char *s = argv[3]; *s; s++)
        x = (x ^ (unsigned char)*s) * 0x100000001B3ULL;
    if (!x)
        x = 1;
    if (!(f = fopen(argv[1], "wb"))) {
        perror(argv[1]);
        return 1;
    }
    for (uint64_t off = 0; off < size; off += 8) {
        unsigned char b[8];
        uint64_t v;

        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        v = x * 0x2545F4914F6CDD1DULL ^ off;
        for (int i = 0; i < 8; i++)
            b[i] = (unsigned char)(v >> (8 * i));
        if (fwrite(b, 1, size - off < 8 ? size - off : 8, f) != (size - off < 8 ? size - off : 8)) {
            perror(argv[1]);
            return 1;
        }
    }
    if (fclose(f)) {
        perror(argv[1]);
        return 1;
    }
    return 0;
}
