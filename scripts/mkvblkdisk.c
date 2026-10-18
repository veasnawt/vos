/*
 * mkvblkdisk: write the deterministic raw disk image for the N3.1 virtio-blk
 * tests (host tool, built by tests/x86_64/vblk.mk).
 *
 * usage: mkvblkdisk OUTPUT SECTORS
 *
 * Every 512-byte sector s of the SECTORS sectors holds
 *   bytes 0-7    "VOSBLK01"
 *   bytes 8-15   s, little endian
 *   bytes 16-23  SECTORS, little endian
 *   bytes 24-511 a xorshift64* stream seeded from s (different in every sector)
 * The same file is also delivered to the kernel as a GRUB module, the
 * fixture against which every byte read through the driver is compared.
 * The output is a pure function of SECTORS (no time, host or user data).
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

int main(int argc, char **argv)
{
    char *end;
    unsigned long long n;
    FILE *f;

    if (argc != 3) {
        fprintf(stderr, "usage: mkvblkdisk OUTPUT SECTORS\n");
        return 2;
    }
    errno = 0;
    n = strtoull(argv[2], &end, 10);
    if (errno || *end || n == 0 || n > 1u << 20) {
        fprintf(stderr, "mkvblkdisk: SECTORS must be 1..1048576\n");
        return 2;
    }
    if (!(f = fopen(argv[1], "wb"))) {
        perror(argv[1]);
        return 1;
    }
    for (uint64_t s = 0; s < n; s++) {
        unsigned char b[512];
        uint64_t x = (s + 1) * 0x9E3779B97F4A7C15ULL ^ 0xD1B54A32D192ED03ULL;

        memcpy(b, "VOSBLK01", 8);
        put64(b + 8, s);
        put64(b + 16, n);
        for (int i = 24; i < 512; i += 8) {
            x ^= x >> 12;
            x ^= x << 25;
            x ^= x >> 27;
            put64(b + i, x * 0x2545F4914F6CDD1DULL);
        }
        if (fwrite(b, 1, sizeof(b), f) != sizeof(b)) {
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
