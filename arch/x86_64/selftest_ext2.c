/*
 * Read-only ext2 reader (N3.3), VOS_TEST only.
 *
 * Every image checks the parts that need no fixture (SHA-256 vectors, a
 * refused open on an invalid device, a clean audit). VOS_TEST_CASE=ext2
 * boots with the ext2 fixtures of scripts/mkext2fixtures.sh as GRUB modules
 * ("ext2-NAME.img" and "ext2-NAME.manifest") and one of them attached
 * through virtio-blk:
 *
 *   - Each image is served by a test-only block device backed by its GRUB
 *     module ("moddev"), read through the block layer and the cache. Every
 *     line of its manifest - written on the host from the source tree with
 *     find, stat, readlink and sha256sum, never from the image - is checked:
 *     geometry, every directory's entries and types, every file's size,
 *     link count, mode and SHA-256 (hashed ranges for sparse files), every
 *     symbolic link's target, FIFOs.
 *   - Path, argument and type errors on the 1 KiB image.
 *   - Corruption: the moddev overlays chosen bytes of a valid image
 *     (superblock, group descriptors, bitmaps, inodes, directory records,
 *     block pointers), truncates it, or fails a sector; each must be refused
 *     with its own error and diagnostic, without a panic, a leaked reader
 *     or an inconsistent audit.
 *   - Device reset and removal (EXT2_ESTALE), repeated open/close,
 *     concurrent readers, and the virtio-blk device with user programs
 *     running.
 */
#include "selftest.h"
#include "blockdev.h"
#include "console.h"
#include "ext2.h"
#include "irq.h"
#include "lib.h"
#include "loader.h"
#include "memlayout.h"
#include "pmm.h"
#include "process.h"
#include "thread.h"
#include "virtio_blk.h"

static int failures, cases;
static const struct boot_info *mbi;

static void expect(int ok, const char *what)
{
    cases++;
    if (ok)
        return;
    failures++;
    serial_puts("selftest: FAIL: ");
    serial_puts(what);
    serial_puts("\r\n");
}

static int streq(const char *a, const char *b)
{
    return a && b && strlen(a) == strlen(b) && memcmp(a, b, strlen(a)) == 0;
}

/* ---- SHA-256 (FIPS 180-4), for comparing with the host's sha256sum ---- */

struct sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;
    unsigned n;
};

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, unsigned n)
{
    return x >> n | x << (32 - n);
}

static void sha_block(struct sha256 *s, const uint8_t *p)
{
    uint32_t w[64], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6],
             h = s->h[7];

    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] +
               (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ w[i - 2] >> 10);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

static void sha_init(struct sha256 *s)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->n = 0;
}

static void sha_update(struct sha256 *s, const uint8_t *p, uint64_t n)
{
    s->len += n;
    while (n) {
        unsigned k = 64 - s->n < n ? 64 - s->n : (unsigned)n;

        memcpy(s->buf + s->n, p, k);
        s->n += k;
        p += k;
        n -= k;
        if (s->n == 64) {
            sha_block(s, s->buf);
            s->n = 0;
        }
    }
}

/* Finish into 64 lowercase hex digits and a NUL. */
static void sha_hex(struct sha256 *s, char *out)
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, lenb[8];

    sha_update(s, &pad, 1);
    while (s->n != 56)
        sha_update(s, &zero, 1);
    for (int i = 0; i < 8; i++)
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, lenb, 8);
    for (int i = 0; i < 32; i++) {
        uint8_t v = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
        out[2 * i] = "0123456789abcdef"[v >> 4];
        out[2 * i + 1] = "0123456789abcdef"[v & 15];
    }
    out[64] = 0;
}

static void test_sha(void)
{
    struct sha256 s;
    char hex[65];

    sha_init(&s);
    sha_hex(&s, hex);
    expect(streq(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "SHA-256 of \"\"");
    sha_init(&s);
    sha_update(&s, (const uint8_t *)"abc", 3);
    sha_hex(&s, hex);
    expect(streq(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 of \"abc\"");
    sha_init(&s);
    sha_update(&s, (const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    sha_hex(&s, hex);
    expect(streq(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
           "SHA-256 of the two-block FIPS vector");
}

#ifdef VOS_TEST_CASE_ext2
/* ---- the module-backed test device ---- */

#define MOD_PATCHES 8

static struct moddev {
    const uint8_t *img;
    uint64_t bytes, sectors;
    int state, fail_on, yield;
    uint64_t epoch, fail_sector, reads;
    unsigned npatch;
    struct {
        uint64_t off;
        uint32_t len;
        uint8_t b[8];
    } patch[MOD_PATCHES];
} md;

static void md_info(void *ctx, struct blk_info *o)
{
    (void)ctx;
    o->state = md.state;
    o->sectors = md.sectors;
    o->max_sectors = BLK_LINE_SECTORS;
    o->caps = BLK_CAP_READ;
    o->epoch = md.epoch;
}

static int md_read(void *ctx, uint64_t lba, uint32_t count, void *buf, uint64_t cap)
{
    uint8_t *out = buf;

    (void)ctx;
    md.reads++;
    if (count == 0 || count > BLK_LINE_SECTORS || cap < (uint64_t)count * BLK_SECTOR_SIZE || lba >= md.sectors ||
        count > md.sectors - lba)
        return BLK_EINVAL;
    if (md.yield)
        thread_yield();
    if (md.fail_on && md.fail_sector >= lba && md.fail_sector < lba + count)
        return BLK_EIO;
    for (uint32_t i = 0; i < count * BLK_SECTOR_SIZE; i++) {
        uint64_t o = lba * BLK_SECTOR_SIZE + i;
        out[i] = o < md.bytes ? md.img[o] : 0;
    }
    for (unsigned p = 0; p < md.npatch; p++)
        for (uint32_t i = 0; i < md.patch[p].len; i++) {
            uint64_t o = md.patch[p].off + i;
            if (o >= lba * BLK_SECTOR_SIZE && o < (lba + count) * BLK_SECTOR_SIZE)
                out[o - lba * BLK_SECTOR_SIZE] = md.patch[p].b[i];
        }
    return BLK_OK;
}

static const struct blk_ops md_ops = { "moddev", md_info, md_read, 0 };

static const struct boot_module *module(const char *name)
{
    for (uint32_t i = 0; i < mbi->module_count; i++)
        if (streq(mbi->modules[i].cmdline, name))
            return &mbi->modules[i];
    return NULL;
}

/* Serve image module `name` (no patches); 0 or the registered ID. */
static blk_id_t md_attach(const char *name)
{
    const struct boot_module *m = module(name);
    blk_id_t id;

    memset(&md, 0, sizeof(md));
    if (!m)
        return 0;
    md.img = phys_to_virt(m->start);
    md.bytes = m->end - m->start;
    md.sectors = md.bytes / BLK_SECTOR_SIZE;
    md.state = BLK_DEV_READY;
    md.epoch = 1;
    return blk_register(name, &md_ops, 0, &id) == BLK_OK ? id : 0;
}

static void md_patch(uint64_t off, uint32_t len, uint64_t value)
{
    if (md.npatch == MOD_PATCHES || len > 8)
        return;
    md.patch[md.npatch].off = off;
    md.patch[md.npatch].len = len;
    for (uint32_t i = 0; i < len; i++)
        md.patch[md.npatch].b[i] = (uint8_t)(value >> (8 * i));
    md.npatch++;
}

/* ---- manifests ---- */

#define MAX_FIELDS 7

struct mline {
    char kind;
    unsigned n;
    const char *f[MAX_FIELDS];
    unsigned len[MAX_FIELDS];
};

static const char *man;
static uint64_t man_len;

/* The line starting at *pos, split at single spaces; *pos moves to the next line. 0 at the end. */
static int next_line(uint64_t *pos, struct mline *l)
{
    if (*pos >= man_len)
        return 0;
    memset(l, 0, sizeof(*l));
    uint64_t i = *pos;
    while (i < man_len && man[i] != '\n') {
        uint64_t s = i;

        while (i < man_len && man[i] != ' ' && man[i] != '\n')
            i++;
        if (l->n < MAX_FIELDS) {
            l->f[l->n] = man + s;
            l->len[l->n] = (unsigned)(i - s);
            l->n++;
        }
        if (i < man_len && man[i] == ' ')
            i++;
    }
    *pos = i + 1;
    l->kind = l->n ? l->f[0][0] : 0;
    return 1;
}

static uint64_t num(const struct mline *l, unsigned k)
{
    uint64_t v = 0;

    for (unsigned i = 0; k < l->n && i < l->len[k]; i++)
        v = v * 10 + (uint64_t)(l->f[k][i] - '0');
    return v;
}

static uint32_t octal(const struct mline *l, unsigned k)
{
    uint32_t v = 0;

    for (unsigned i = 0; k < l->n && i < l->len[k]; i++)
        v = v * 8 + (uint32_t)(l->f[k][i] - '0');
    return v;
}

static int field_is(const struct mline *l, unsigned k, const char *s, unsigned n)
{
    return k < l->n && l->len[k] == n && memcmp(l->f[k], s, n) == 0;
}

/* The path (last field) as a NUL-terminated string. */
static char pathbuf[EXT2_PATH_MAX + 1];

static const char *path_of(const struct mline *l)
{
    unsigned n = l->len[l->n - 1] <= EXT2_PATH_MAX ? l->len[l->n - 1] : EXT2_PATH_MAX;

    memcpy(pathbuf, l->f[l->n - 1], n);
    pathbuf[n] = 0;
    return pathbuf;
}

/* Kind of the manifest entry with path p (0 if none). */
static char kind_of(const char *p, unsigned plen)
{
    uint64_t pos = 0;
    struct mline l;

    while (next_line(&pos, &l))
        if (l.kind != 'S' && l.kind != 'R' && l.len[l.n - 1] == plen && memcmp(l.f[l.n - 1], p, plen) == 0)
            return l.kind;
    return 0;
}

static int set_manifest(const char *name)
{
    const struct boot_module *m = module(name);

    if (!m)
        return 0;
    man = phys_to_virt(m->start);
    man_len = m->end - m->start;
    return 1;
}

/* ---- checking an image against its manifest ---- */

static uint8_t data[65536];
static char hex[65];

/* SHA-256 of bytes [off, off + len) of file ino, read in chunks of `chunk` bytes. */
static int hash_file(struct ext2_fs *fs, uint32_t ino, uint64_t off, uint64_t len, uint64_t chunk)
{
    struct sha256 s;
    uint64_t done;

    sha_init(&s);
    while (len) {
        uint64_t want = len < chunk ? len : chunk;

        if (ext2_read(fs, ino, off, data, want, &done) != EXT2_OK || done != want)
            return 0;
        sha_update(&s, data, done);
        off += done;
        len -= done;
    }
    sha_hex(&s, hex);
    return 1;
}

static int walk_yield;                  /* yield after every manifest line (concurrency runs) */

static uint8_t want_type(char kind)
{
    return kind == 'F' ? EXT2_T_REG : kind == 'D' ? EXT2_T_DIR : kind == 'L' ? EXT2_T_LNK :
           kind == 'P' ? EXT2_T_FIFO : 0xFF;
}

/* Every manifest line against the file system; returns the number of mismatches. */
static unsigned check_manifest(struct ext2_fs *fs, unsigned *checked)
{
    static const uint64_t chunks[] = { 7, 512, 1000, 4096, 65536 };
    struct ext2_info info;
    struct mline l;
    uint64_t pos = 0;
    unsigned bad = 0, n = 0;

    if (ext2_info(fs, &info) != EXT2_OK)
        return 1;
    while (next_line(&pos, &l)) {
        struct ext2_stat st;
        uint32_t ino = 0;
        int ok = 1;

        n++;
        if (l.kind == 'S') {
            uint64_t v = num(&l, 2);
            ok = field_is(&l, 1, "block_size", 10) ? v == info.block_size : field_is(&l, 1, "blocks", 6) ?
                 v == info.blocks : field_is(&l, 1, "inodes", 6) ? v == info.inodes :
                 field_is(&l, 1, "inode_size", 10) ? v == info.inode_size :
                 field_is(&l, 1, "blocks_per_group", 16) ? v == info.blocks_per_group :
                 field_is(&l, 1, "inodes_per_group", 16) ? v == info.inodes_per_group :
                 field_is(&l, 1, "first_block", 11) ? v == info.first_data_block :
                 field_is(&l, 1, "groups", 6) ? v == info.groups : 0;
        } else if (ext2_resolve(fs, EXT2_ROOT_INO, path_of(&l), &ino) != EXT2_OK ||
                   ext2_stat(fs, ino, &st) != EXT2_OK || (l.kind != 'R' && st.type != want_type(l.kind))) {
            ok = 0;
        } else if (l.kind == 'F') {
            ok = st.size == num(&l, 1) && st.links == num(&l, 2) && (st.mode & 0777) == octal(&l, 3);
            if (ok && !field_is(&l, 4, "-", 1))
                ok = hash_file(fs, ino, 0, st.size, chunks[n % 5]) && l.len[4] == 64 &&
                     memcmp(hex, l.f[4], 64) == 0;
            uint64_t done = 1;
            ok &= ext2_read(fs, ino, st.size, data, 16, &done) == EXT2_OK && done == 0;
        } else if (l.kind == 'R') {
            ok = hash_file(fs, ino, num(&l, 1), num(&l, 2), 4096) && l.len[3] == 64 && memcmp(hex, l.f[3], 64) == 0;
        } else if (l.kind == 'L') {
            size_t len = 0;
            ok = st.size == num(&l, 1) && ext2_readlink(fs, ino, (char *)data, sizeof(data), &len) == EXT2_OK &&
                 len == l.len[2] && memcmp(data, l.f[2], len) == 0;
        } else if (l.kind == 'P') {
            uint64_t done;
            ok = ext2_read(fs, ino, 0, data, 1, &done) == EXT2_ENOTREG;
        } else if (l.kind == 'D') {
            static struct ext2_dirent e;
            static char child[EXT2_PATH_MAX + 1];
            uint64_t cookie = 0;
            unsigned children = 0, k = 0, plen = l.len[l.n - 1];
            uint32_t self = ino;

            ok = (st.mode & 0777) == octal(&l, 2);
            memcpy(child, l.f[l.n - 1], plen);
            while (ok && ext2_readdir(fs, self, &cookie, &e) == EXT2_OK) {
                if (k == 0)
                    ok = streq(e.name, ".") && e.ino == self;
                else if (k == 1)
                    ok = streq(e.name, "..");
                else {
                    unsigned cl = plen == 1 ? 1 : plen + 1;
                    if (plen > 1)
                        child[plen] = '/';
                    memcpy(child + cl, e.name, e.name_len);
                    child[cl + e.name_len] = 0;
                    char kind = kind_of(child, cl + e.name_len);
                    uint32_t cino;
                    ok = kind && (e.type == EXT2_T_UNKNOWN || e.type == want_type(kind)) &&
                         ext2_lookup(fs, self, e.name, e.name_len, &cino) == EXT2_OK && cino == e.ino;
                    children++;
                }
                k++;
            }
            ok &= children == num(&l, 1);
        }
        if (walk_yield)
            thread_yield();
        if (!ok) {
            bad++;
            serial_puts("selftest: ext2 manifest mismatch: ");
            for (unsigned i = 0; i < l.n; i++) {
                for (unsigned j = 0; j < l.len[i] && j < 80; j++) {
                    char c[2] = { l.f[i][j], 0 };
                    serial_puts(c);
                }
                serial_puts(" ");
            }
            serial_puts("\r\n");
        }
    }
    *checked = n;
    return bad;
}

static void test_image(const char *img, const char *manifest)
{
    struct ext2_fs *fs;
    unsigned checked = 0, bad = 1;
    blk_id_t id = md_attach(img);
    int err = id ? ext2_open(id, &fs) : -1;

    if (id && err == EXT2_OK && set_manifest(manifest)) {
        bad = check_manifest(fs, &checked);
        ext2_close(fs);
    }
    if (id)
        blk_unregister(id);
    serial_puts("ext2: ");
    serial_puts(img);
    serial_puts(": ");
    serial_dec(checked);
    serial_puts(" manifest lines checked, ");
    serial_dec(bad);
    serial_puts(" mismatches\r\n");
    expect(err == EXT2_OK && checked > 0 && bad == 0, "image matches its host-side manifest exactly");
}

/* ---- errors on a valid file system ---- */

static const uint8_t konst[16] = { 1 };

static void test_errors(void)
{
    struct ext2_fs *fs, *fs2;
    blk_id_t id = md_attach("ext2-1k.img");
    uint32_t ino, ino2, deep, a;
    struct ext2_stat st;
    struct ext2_dirent e;
    uint64_t done, cookie;
    size_t len;
    static char longpath[EXT2_PATH_MAX + 64], name[300];

    if (!id || ext2_open(id, &fs) != EXT2_OK) {
        expect(0, "1 KiB image opens");
        return;
    }
    expect(ext2_resolve(fs, 0, "/", &ino) == EXT2_OK && ino == EXT2_ROOT_INO &&
           ext2_resolve(fs, EXT2_ROOT_INO, "..", &ino2) == EXT2_OK && ino2 == EXT2_ROOT_INO &&
           ext2_resolve(fs, EXT2_ROOT_INO, "/./../.", &ino2) == EXT2_OK && ino2 == EXT2_ROOT_INO,
           "\"/\", \"..\" of the root and \"/./../.\" are the root");
    expect(ext2_resolve(fs, 0, "/deep/a", &deep) == EXT2_OK &&
           ext2_resolve(fs, deep, "b/c/../c/d/e/f/g/h/leaf.txt", &a) == EXT2_OK &&
           ext2_resolve(fs, 0, "/deep/a/b/c/d/e/f/g/h/leaf.txt", &ino) == EXT2_OK && a == ino,
           "relative resolution with \"..\" reaches the same inode as the absolute path");
    expect(ext2_resolve(fs, 0, "/hello.txt", &ino) == EXT2_OK && ext2_resolve(fs, 0, "/hello-hardlink.txt", &ino2) ==
           EXT2_OK && ino == ino2 && ext2_stat(fs, ino, &st) == EXT2_OK && st.links == 2,
           "a hard link is the same inode, with two links");
    expect(ext2_resolve(fs, 0, "/dup/x/name.txt", &ino) == EXT2_OK && ext2_resolve(fs, 0, "/dup/y/name.txt", &ino2) ==
           EXT2_OK && ext2_resolve(fs, 0, "/name.txt", &a) == EXT2_OK && ino != ino2 && ino != a && ino2 != a,
           "the same name in three directories names three different files");
    memset(name, 'n', sizeof(name));
    name[0] = 'l';
    expect(ext2_lookup(fs, EXT2_ROOT_INO, name, 255, &ino) == EXT2_ENOENT &&
           ext2_lookup(fs, EXT2_ROOT_INO, name, 256, &ino) == EXT2_ENAMETOOLONG &&
           ext2_lookup(fs, EXT2_ROOT_INO, "a/b", 3, &ino) == EXT2_EINVAL &&
           ext2_lookup(fs, EXT2_ROOT_INO, "", 0, &ino) == EXT2_EINVAL, "lookup: 255 bytes allowed, 256 too long, '/' and empty refused");
    expect(ext2_resolve(fs, 0, "/nonexistent", &ino) == EXT2_ENOENT &&
           ext2_resolve(fs, 0, "/hello.txt/x", &ino) == EXT2_ENOTDIR &&
           ext2_resolve(fs, 0, "/link-dir/b", &ino) == EXT2_ELOOP &&
           ext2_resolve(fs, 0, "/link-dir", &ino) == EXT2_OK && ext2_stat(fs, ino, &st) == EXT2_OK &&
           st.type == EXT2_T_LNK, "missing names, files used as directories, and symbolic links are not followed");
    expect(ext2_resolve(fs, 0, "", &ino) == EXT2_EINVAL && ext2_resolve(fs, 0, "//hello.txt", &ino) == EXT2_EINVAL &&
           ext2_resolve(fs, 0, "/deep//a", &ino) == EXT2_EINVAL && ext2_resolve(fs, 0, "/deep/", &ino) == EXT2_EINVAL,
           "empty paths and empty components refused");
    memset(longpath, 0, sizeof(longpath));
    for (unsigned i = 0; i < 65; i++)
        memcpy(longpath + 2 * i, "./", 2);
    longpath[129] = 0;
    expect(ext2_resolve(fs, EXT2_ROOT_INO, longpath, &ino) == EXT2_ETOODEEP, "65 components: too deep");
    memset(longpath, 'a', EXT2_PATH_MAX + 1);
    longpath[EXT2_PATH_MAX + 1] = 0;
    expect(ext2_resolve(fs, 0, longpath, &ino) == EXT2_ENAMETOOLONG, "a path above 1024 bytes is too long");
    memset(longpath, 'a', 256);
    longpath[256] = 0;
    expect(ext2_resolve(fs, EXT2_ROOT_INO, longpath, &ino) == EXT2_ENAMETOOLONG, "a 256-byte component is too long");
    expect(ext2_resolve(fs, 0, "/deep", &deep) == EXT2_OK && ext2_read(fs, deep, 0, data, 1, &done) == EXT2_EISDIR &&
           ext2_resolve(fs, 0, "/hello.txt", &ino) == EXT2_OK && ext2_readlink(fs, ino, (char *)data, 99, &len) ==
           EXT2_ENOTREG && ext2_readdir(fs, ino, &(uint64_t){ 0 }, &e) == EXT2_ENOTDIR,
           "read on a directory, readlink on a file, readdir on a file refused");
    expect(ext2_read(fs, ino, 0, (void *)konst, 4, &done) == EXT2_EINVAL &&
           ext2_read(fs, ino, 0, (void *)0x400000, 4, &done) == EXT2_EINVAL &&
           ext2_read(fs, ino, ~0ULL, data, 4, &done) == EXT2_OK && done == 0 &&
           ext2_read(fs, ino, 5, data, sizeof(data), &done) == EXT2_OK && done == 27 &&
           ext2_read(fs, ino, 0, data, ~0ULL, &done) == EXT2_EINVAL,
           "read: read-only, user and wrapping buffers refused; past the end 0 bytes; length clipped to the file");
    expect(ext2_stat(fs, 0, &st) == EXT2_EINVAL && ext2_stat(fs, 1, &st) == EXT2_EINVAL &&
           ext2_stat(fs, 7, &st) == EXT2_EINVAL && ext2_stat(fs, 513, &st) == EXT2_EINVAL &&
           ext2_stat(fs, 500, &st) == EXT2_ECORRUPT,
           "inode 0, reserved inodes and inodes past the count refused; an unused inode is reported");
    cookie = 3;
    expect(ext2_readdir(fs, EXT2_ROOT_INO, &cookie, &e) == EXT2_EINVAL, "readdir: a cookie inside a record is refused");
    expect(ext2_resolve(fs, 0, "/link-long", &ino) == EXT2_OK && ext2_readlink(fs, ino, (char *)data, 94, &len) ==
           EXT2_ENAMETOOLONG, "readlink into a buffer shorter than the target refused");
    expect(ext2_open(id, &fs2) == EXT2_OK && fs2 != fs && ext2_open_count() == 2 && ext2_close(fs2) == EXT2_OK,
           "two readers on the same device are independent");
    expect(ext2_close(fs) == EXT2_OK && ext2_close(fs) == EXT2_EINVAL && ext2_stat(fs, 2, &st) == EXT2_EINVAL,
           "a closed reader is refused");
    expect(blk_unregister(id) == BLK_OK && ext2_open_count() == 0 && ext2_audit() == NULL, "no reader left; audit clean");
}

/* ---- corruption ---- */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Geometry of the clean 1 KiB image, from its raw bytes. */
static struct {
    const uint8_t *img;
    uint32_t bs, isz, ipg, blocks, inodes, it0, ib0, root_blk0;
    uint32_t hello, sz12289, deep, deepa, linklong;     /* inode numbers */
} G;

static uint64_t inode_off(uint32_t ino)
{
    uint32_t g = (ino - 1) / G.ipg, idx = (ino - 1) % G.ipg;
    const uint8_t *gd = G.img + 2 * G.bs + g * 32;          /* 1 KiB blocks: descriptors in block 2 */

    return (uint64_t)rd32(gd + 8) * G.bs + (uint64_t)idx * G.isz;
}

/* Offset (in the image) of the record named `name` in the first block of directory dir, or 0. */
static uint64_t dir_entry(uint32_t dir, const char *name)
{
    uint64_t base = (uint64_t)rd32(G.img + inode_off(dir) + 0x28) * G.bs;

    for (uint32_t pos = 0; pos < G.bs; pos += rd16(G.img + base + pos + 4)) {
        const uint8_t *e = G.img + base + pos;
        if (rd16(e + 4) < 8)
            return 0;
        if (e[6] == strlen(name) && memcmp(e + 8, name, e[6]) == 0)
            return base + pos;
    }
    return 0;
}

static uint64_t root_entry(const char *name)
{
    return dir_entry(2, name);
}

static int learn_geometry(void)
{
    const struct boot_module *m = module("ext2-1k.img");
    struct ext2_fs *fs;
    blk_id_t id;
    int ok;

    if (!m)
        return 0;
    G.img = phys_to_virt(m->start);
    G.bs = 1024u << rd32(G.img + 1024 + 0x18);
    G.isz = rd16(G.img + 1024 + 0x58);
    G.ipg = rd32(G.img + 1024 + 0x28);
    G.blocks = rd32(G.img + 1024 + 0x04);
    G.inodes = rd32(G.img + 1024 + 0x00);
    G.it0 = rd32(G.img + 2 * G.bs + 8);
    G.ib0 = rd32(G.img + 2 * G.bs + 4);
    G.root_blk0 = rd32(G.img + inode_off(2) + 0x28);
    if (!(id = md_attach("ext2-1k.img")) || ext2_open(id, &fs) != EXT2_OK)
        return 0;
    ok = ext2_resolve(fs, 0, "/hello.txt", &G.hello) == EXT2_OK &&
         ext2_resolve(fs, 0, "/sizes/size-12289", &G.sz12289) == EXT2_OK &&
         ext2_resolve(fs, 0, "/deep", &G.deep) == EXT2_OK && ext2_resolve(fs, 0, "/deep/a", &G.deepa) == EXT2_OK &&
         ext2_resolve(fs, 0, "/link-long", &G.linklong) == EXT2_OK;
    ext2_close(fs);
    blk_unregister(id);
    return ok && G.bs == 1024 && G.root_blk0;
}

struct corruption {
    const char *what;
    int at_open;                /* refused by ext2_open */
    int err;
    const char *problem;
};

/*
 * Run one corruption: the patches are set by `setup` on the fresh moddev;
 * then open, and unless refused there, `probe` (an operation on the
 * reader). Checks the error, the diagnostic, and that nothing is left over.
 */
static unsigned corruptions_run;

static void corrupt_case(const struct corruption *c, void (*setup)(void), int (*probe)(struct ext2_fs *))
{
    struct ext2_fs *fs = NULL;
    blk_id_t id = md_attach("ext2-1k.img");
    int err, ok;

    if (!id) {
        expect(0, c->what);
        return;
    }
    setup();
    err = ext2_open(id, &fs);
    if (c->at_open) {
        ok = err == c->err && fs == NULL;
    } else {
        ok = err == EXT2_OK && probe(fs) == c->err;
        if (fs)
            ok &= ext2_close(fs) == EXT2_OK;
    }
    if (c->err != EXT2_OK)              /* the diagnostic is only set on failure */
        ok &= streq(ext2_problem(), c->problem);
    if (!ok) {
        serial_puts("selftest: ext2 corruption: got ");
        serial_puts(ext2_strerror(err));
        serial_puts(" / ");
        serial_puts(ext2_problem());
        serial_puts("\r\n");
    }
    ok &= blk_unregister(id) == BLK_OK && ext2_open_count() == 0 && ext2_audit() == NULL && blk_audit() == NULL;
    corruptions_run++;
    expect(ok, c->what);
}

#define SB(off) (1024 + (off))
static void c_magic(void) { md_patch(SB(0x38), 2, 0x1234); }
static void c_rev0(void) { md_patch(SB(0x4C), 4, 0); }
static void c_bsize(void) { md_patch(SB(0x18), 4, 3); }
static void c_bsize_huge(void) { md_patch(SB(0x18), 4, 0xFFFFFFFF); }
static void c_extents(void) { md_patch(SB(0x60), 4, rd32(G.img + SB(0x60)) | 0x40); }
static void c_unknown_incompat(void) { md_patch(SB(0x60), 4, rd32(G.img + SB(0x60)) | 0x80000000u); }
static void c_csum(void) { md_patch(SB(0x64), 4, rd32(G.img + SB(0x64)) | 0x400); }
static void c_journal(void) { md_patch(SB(0x5C), 4, rd32(G.img + SB(0x5C)) | 0x4); }
static void c_state(void) { md_patch(SB(0x3A), 2, 2); }
static void c_isize(void) { md_patch(SB(0x58), 2, 100); }
static void c_bpg0(void) { md_patch(SB(0x20), 4, 0); md_patch(SB(0x24), 4, 0); }
static void c_ipg0(void) { md_patch(SB(0x28), 4, 0); }
static void c_icount(void) { md_patch(SB(0x00), 4, G.inodes + 1); }
static void c_groups(void) { md_patch(SB(0x04), 4, 0xFFFFFFF0u); }
static void c_fdb(void) { md_patch(SB(0x14), 4, 0); }
static void c_truncated(void) { md.sectors = md.sectors / 2; }
static void c_gd_table(void) { md_patch(2 * G.bs + 8, 4, G.blocks + 5); }
static void c_gd_bitmap(void) { md_patch(2 * G.bs + 32 + 0, 4, G.ib0); }
static void c_root_mode(void) { md_patch(inode_off(2), 2, 0x81A4); }
static void c_root_unused(void) { md_patch((uint64_t)G.ib0 * G.bs, 1, G.img[(uint64_t)G.ib0 * G.bs] & ~2u); }
static void c_rec0(void) { md_patch((uint64_t)G.root_blk0 * G.bs + 4, 2, 0); }
static void c_rec13(void) { md_patch((uint64_t)G.root_blk0 * G.bs + 4, 2, 13); }
static void c_recpast(void) { md_patch((uint64_t)G.root_blk0 * G.bs + 12 + 4, 2, (uint64_t)G.bs); }
static void c_namelen(void) { md_patch((uint64_t)G.root_blk0 * G.bs + 6, 1, 200); }
static void c_notdot(void) { md_patch((uint64_t)G.root_blk0 * G.bs + 8, 1, 'x'); }
static void c_ent_ino(void) { md_patch(root_entry("hello.txt"), 4, G.inodes + 1); }
static void c_ent_slash(void) { md_patch(root_entry("hello.txt") + 8 + 2, 1, '/'); }
static void c_ent_type(void) { md_patch(root_entry("hello.txt") + 7, 1, 2); }
static void c_ent_reserved(void) { md_patch(root_entry("hello.txt"), 4, 7); }
static void c_flags(void) { md_patch(inode_off(G.hello) + 0x20, 4, 0x80000); }
static void c_links0(void) { md_patch(inode_off(G.hello) + 0x1A, 2, 0); }
static void c_iblocks(void) { md_patch(inode_off(G.hello) + 0x1C, 4, 1); }
static void c_size_huge(void) { md_patch(inode_off(G.sz12289) + 0x6C, 4, 0xFFFF); }
static void c_dirsize(void) { md_patch(inode_off(G.deep) + 0x04, 4, 1000); }
static void c_ptr_out(void) { md_patch(inode_off(G.hello) + 0x28, 4, G.blocks + 100); }
static void c_ptr_meta(void) { md_patch(inode_off(G.hello) + 0x28, 4, G.it0); }
static void c_ind_meta(void) { md_patch(inode_off(G.sz12289) + 0x28 + 12 * 4, 4, G.it0 + 1); }
static void c_ind_entry(void)
{
    uint32_t ind = rd32(G.img + inode_off(G.sz12289) + 0x28 + 12 * 4);
    md_patch((uint64_t)ind * G.bs, 4, G.blocks + 7);
}
static void c_dir_hole(void) { md_patch(inode_off(G.deep) + 0x28, 4, 0); }
static void c_cycle(void) { md_patch(dir_entry(G.deepa, "b"), 4, G.deep); }     /* /deep/a/b -> /deep */
static void c_ptr_rgdt(void) { md_patch(inode_off(G.hello) + 0x28, 4, 3); }       /* reserved GDT blocks */
static void c_ptr_backup(void) { md_patch(inode_off(G.hello) + 0x28, 4, 3073); } /* group 3's superblock copy */
static void c_link_size(void) { md_patch(inode_off(G.linklong) + 0x04, 4, G.bs); }
static void c_hole_file(void) { md_patch(inode_off(G.hello) + 0x28, 4, 0); }
static void c_ioerr_root(void)
{
    md.fail_on = 1;
    md.fail_sector = inode_off(2) / BLK_SECTOR_SIZE;
}
static void c_ioerr_data(void)
{
    md.fail_on = 1;
    md.fail_sector = (uint64_t)rd32(G.img + inode_off(G.sz12289) + 0x28 + 5 * 4) * (G.bs / BLK_SECTOR_SIZE);
}

static uint32_t ino_tmp;
static uint64_t done_tmp;
static int p_hello(struct ext2_fs *fs) { return ext2_resolve(fs, 0, "/hello.txt", &ino_tmp); }
static int p_stat_hello(struct ext2_fs *fs) { struct ext2_stat st; return ext2_stat(fs, G.hello, &st); }
static int p_read_hello(struct ext2_fs *fs) { return ext2_read(fs, G.hello, 0, data, 32, &done_tmp); }
static int p_read_12289(struct ext2_fs *fs) { return ext2_read(fs, G.sz12289, 0, data, 12289, &done_tmp); }
static int p_stat_12289(struct ext2_fs *fs) { struct ext2_stat st; return ext2_stat(fs, G.sz12289, &st); }
static int p_deep(struct ext2_fs *fs) { return ext2_resolve(fs, 0, "/deep/a", &ino_tmp); }
static int p_cycle(struct ext2_fs *fs)
{
    static char path[EXT2_PATH_MAX];
    uint32_t n = 0, ino;

    memcpy(path, "/deep", 5);
    n = 5;
    for (unsigned i = 0; i < 40 && n + 4 < sizeof(path); i++) {
        memcpy(path + n, "/a/b", 4);    /* each "a/b" comes back to /deep through the forged entry */
        n += 4;
    }
    path[n] = 0;
    int err = ext2_resolve(fs, 0, "/deep/a/b/a/b/a", &ino);
    if (err || ino != G.deepa)
        return EXT2_EIO;                /* the cycle itself must resolve: it is a valid (forged) link */
    return ext2_resolve(fs, 0, path, &ino);
}
static int p_link(struct ext2_fs *fs) { size_t len; return ext2_readlink(fs, G.linklong, (char *)data, 4096, &len); }
static int p_hole(struct ext2_fs *fs)
{
    static const uint8_t zero[32];
    int err = ext2_read(fs, G.hello, 0, data, 32, &done_tmp);
    return err ? err : done_tmp == 32 && memcmp(data, zero, 32) == 0 ? EXT2_OK : EXT2_EIO;
}

static void test_corruption(void)
{
    static const struct corruption C[] = {
        { "bad magic: not ext2", 1, EXT2_ENOTEXT2, "no ext2 magic number" },
        { "revision 0 refused", 1, EXT2_EUNSUPP, "revision other than 1 (dynamic)" },
        { "8 KiB blocks refused", 1, EXT2_EUNSUPP, "block size above 4 KiB" },
        { "a block-size exponent of 2^32-1 refused (no shift overflow)", 1, EXT2_EUNSUPP, "block size above 4 KiB" },
        { "extents feature refused", 1, EXT2_EUNSUPP, "incompatible feature outside the profile" },
        { "unknown incompatible feature refused", 1, EXT2_EUNSUPP, "incompatible feature outside the profile" },
        { "metadata_csum refused", 1, EXT2_EUNSUPP, "read-only-compatible feature outside the profile" },
        { "a journal (ext3) refused", 1, EXT2_EUNSUPP, "compatible feature outside the profile (e.g. a journal)" },
        { "errors recorded: refused", 1, EXT2_EUNSUPP, "not cleanly unmounted (errors or orphans recorded)" },
        { "inode size 100 refused", 1, EXT2_EUNSUPP, "inode size other than 128 or 256" },
        { "zero blocks per group", 1, EXT2_ECORRUPT, "invalid blocks per group" },
        { "zero inodes per group", 1, EXT2_ECORRUPT, "invalid inodes per group" },
        { "inode count inconsistent with the groups", 1, EXT2_ECORRUPT, "inode count differs from groups x inodes per group" },
        { "a block count needing millions of groups", 1, EXT2_EUNSUPP, "more block groups than the reader supports" },
        { "first data block wrong for 1 KiB blocks", 1, EXT2_ECORRUPT, "first data block inconsistent with the block size" },
        { "truncated image (device half the file system)", 1, EXT2_ECORRUPT, "file system larger than the device (truncated image)" },
        { "group descriptor: inode table beyond the file system", 1, EXT2_ECORRUPT, "group bitmap or inode table outside its group" },
        { "group descriptor: group 1's block bitmap in group 0", 1, EXT2_ECORRUPT, "group bitmap or inode table outside its group" },
        { "root inode not a directory", 1, EXT2_ECORRUPT, "root inode is not a directory" },
        { "root inode not marked in use", 1, EXT2_ECORRUPT, "inode not marked in use in its bitmap" },
        { "directory record length 0 (would not advance)", 1, EXT2_ECORRUPT, "directory record with an invalid length" },
        { "directory record length not a multiple of 4", 1, EXT2_ECORRUPT, "directory record with an invalid length" },
        { "directory record past the block end", 1, EXT2_ECORRUPT, "directory record with an invalid length" },
        { "directory name longer than its record", 1, EXT2_ECORRUPT, "directory name longer than its record" },
        { "directory without \".\" first", 1, EXT2_ECORRUPT, "directory does not start with \".\"" },
    };
    static void (*const setup[])(void) = {
        c_magic, c_rev0, c_bsize, c_bsize_huge, c_extents, c_unknown_incompat, c_csum, c_journal, c_state, c_isize,
        c_bpg0, c_ipg0, c_icount, c_groups, c_fdb, c_truncated, c_gd_table, c_gd_bitmap, c_root_mode, c_root_unused,
        c_rec0, c_rec13, c_recpast, c_namelen, c_notdot,
    };
    static const struct {
        struct corruption c;
        void (*setup)(void);
        int (*probe)(struct ext2_fs *);
    } L[] = {
        { { "root entry names an inode beyond the count (whole root block checked at open)", 1, EXT2_ECORRUPT,
            "directory entry names an inode beyond the inode count" }, c_ent_ino, 0 },
        { { "root entry name containing '/' (checked at open)", 1, EXT2_ECORRUPT,
            "directory entry name contains '/' or NUL" }, c_ent_slash, 0 },
        { { "entry type differs from the inode", 0, EXT2_ECORRUPT, "directory entry type differs from the inode" },
          c_ent_type, p_hello },
        { { "entry names a reserved inode", 0, EXT2_ECORRUPT, "directory entry names a reserved inode" },
          c_ent_reserved, p_hello },
        { { "inode with the extents flag refused", 0, EXT2_EUNSUPP,
            "inode uses extents, inline data, compression or encryption" }, c_flags, p_stat_hello },
        { { "inode in use with no links", 0, EXT2_ECORRUPT, "inode in use without links" }, c_links0, p_stat_hello },
        { { "inode block count not whole blocks", 0, EXT2_ECORRUPT,
            "inode block count not a multiple of the block size" }, c_iblocks, p_stat_hello },
        { { "file size beyond the block map", 0, EXT2_ECORRUPT, "file size beyond what the block map can address" },
          c_size_huge, p_stat_12289 },
        { { "directory size not whole blocks", 0, EXT2_ECORRUPT,
            "directory size not a positive multiple of the block size" }, c_dirsize, p_deep },
        { { "data block pointer past the file system", 0, EXT2_ECORRUPT,
            "block pointer outside the data blocks of the file system" }, c_ptr_out, p_read_hello },
        { { "data block pointer into the inode table", 0, EXT2_ECORRUPT,
            "block pointer outside the data blocks of the file system" }, c_ptr_meta, p_read_hello },
        { { "indirect block in the inode table", 0, EXT2_ECORRUPT,
            "indirect block outside the data blocks of the file system" }, c_ind_meta, p_read_12289 },
        { { "indirect block entry past the file system", 0, EXT2_ECORRUPT,
            "block pointer outside the data blocks of the file system" }, c_ind_entry, p_read_12289 },
        { { "hole in a directory", 0, EXT2_ECORRUPT, "hole in a directory" }, c_dir_hole, p_deep },
        { { "directory cycle: a path around it ends at the depth limit", 0, EXT2_ETOODEEP,
            "path deeper than the limit" }, c_cycle, p_cycle },
        { { "data block pointer into the reserved GDT blocks", 0, EXT2_ECORRUPT,
            "block pointer outside the data blocks of the file system" }, c_ptr_rgdt, p_read_hello },
        { { "data block pointer into group 3's backup superblock (sparse_super)", 0, EXT2_ECORRUPT,
            "block pointer outside the data blocks of the file system" }, c_ptr_backup, p_read_hello },
        { { "symbolic link as long as a block", 0, EXT2_ECORRUPT, "symbolic link with an impossible target length" },
          c_link_size, p_link },
        { { "a zero pointer in a file reads as a hole (zeros)", 0, EXT2_OK, "none" }, c_hole_file, p_hole },
        { { "device error on the root inode: open fails with EIO", 1, EXT2_EIO, "device read failed" }, c_ioerr_root, 0 },
        { { "device error in file data: EIO", 0, EXT2_EIO, "device read failed" }, c_ioerr_data, p_read_12289 },
    };
    uint64_t f0 = pmm_free_count();

    if (!learn_geometry()) {
        expect(0, "geometry of the 1 KiB image learned");
        return;
    }
    for (unsigned i = 0; i < sizeof(C) / sizeof(C[0]); i++)
        corrupt_case(&C[i], setup[i], 0);
    for (unsigned i = 0; i < sizeof(L) / sizeof(L[0]); i++)
        corrupt_case(&L[i].c, L[i].setup, L[i].probe);
    expect(pmm_free_count() == f0 && blk_count() == 1, "after every corruption: no frame and no device left behind");
}

/* ---- device reset, removal, repeated open, concurrency ---- */

static void test_lifecycle(void)
{
    struct ext2_fs *fs;
    struct ext2_stat st;
    uint32_t ino;
    blk_id_t id = md_attach("ext2-1k.img");
    uint64_t f0 = pmm_free_count();
    unsigned cycles = 0;

    expect(id && ext2_open(id, &fs) == EXT2_OK && ext2_resolve(fs, 0, "/hello.txt", &ino) == EXT2_OK,
           "reader open on the module device");
    md.state = BLK_DEV_UNAVAILABLE;
    expect(ext2_stat(fs, ino, &st) == EXT2_EUNAVAIL, "device unavailable: EXT2_EUNAVAIL");
    md.state = BLK_DEV_READY;
    expect(ext2_stat(fs, ino, &st) == EXT2_OK, "available again: the reader works");
    md.epoch++;
    struct ext2_info info;
    expect(ext2_info(fs, &info) == EXT2_ESTALE, "device reset: even ext2_info, which reads nothing, is EXT2_ESTALE");
    expect(ext2_stat(fs, ino, &st) == EXT2_ESTALE, "device reset (epoch changed): EXT2_ESTALE");
    md.epoch--;
    expect(ext2_stat(fs, ino, &st) == EXT2_ESTALE && ext2_close(fs) == EXT2_OK,
           "a stale reader stays stale even if the epoch looks old again; it closes");
    expect(ext2_open(id, &fs) == EXT2_OK && blk_unregister(id) == BLK_OK && ext2_stat(fs, ino, &st) == EXT2_ESTALE &&
           ext2_close(fs) == EXT2_OK, "device removed while open: EXT2_ESTALE, and the reader closes");
    expect(ext2_open(id, &fs) == EXT2_EINVAL && ext2_open_count() == 0, "opening a removed device is refused");

    id = md_attach("ext2-1k.img");
    for (unsigned i = 0; i < 100; i++) {
        struct ext2_fs *f;
        cycles += ext2_open(id, &f) == EXT2_OK && ext2_resolve(f, 0, "/deep/a/b/c/d/e/f/g/h/leaf.txt", &ino) ==
                  EXT2_OK && ext2_close(f) == EXT2_OK;
    }
    struct ext2_fs *many[EXT2_MAX_FS + 1];
    unsigned opened = 0;
    while (opened <= EXT2_MAX_FS && ext2_open(id, &many[opened]) == EXT2_OK)
        opened++;
    int full = ext2_open(id, &fs);
    while (opened)
        ext2_close(many[--opened]);
    expect(cycles == 100 && full == EXT2_EBUSY && ext2_open_count() == 0 && pmm_free_count() == f0 &&
           ext2_audit() == NULL, "100 open/read/close cycles and a full slot table: nothing leaked");
    blk_unregister(id);
}

static volatile unsigned rdone, rbad;
static struct ext2_fs *shared;
static uint8_t tbuf[2][4096];
static char got[2][65];

static void conc_reader(void *arg)
{
    unsigned k = (unsigned)(uint64_t)arg;
    uint32_t ino;
    uint64_t done, off = 0;
    struct sha256 s;
    char h[65];

    sha_init(&s);
    if (ext2_resolve(shared, 0, k ? "/data.bin" : "/sizes/size-12289", &ino) != EXT2_OK)
        rbad++;
    else
        for (;;) {
            if (ext2_read(shared, ino, off, tbuf[k], sizeof(tbuf[k]), &done) != EXT2_OK) {
                rbad++;
                break;
            }
            if (!done)
                break;
            sha_update(&s, tbuf[k], done);
            off += done;
        }
    sha_hex(&s, h);
    memcpy(got[k], h, sizeof(h));       /* compared with the manifest by the caller */
    rdone++;
}

/* The manifest SHA-256 of `path` (or "" if none) into out. */
static void manifest_sha(const char *path, char *out)
{
    uint64_t pos = 0;
    struct mline l;

    out[0] = 0;
    while (next_line(&pos, &l))
        if (l.kind == 'F' && l.len[5] == strlen(path) && memcmp(l.f[5], path, l.len[5]) == 0 && l.len[4] == 64) {
            memcpy(out, l.f[4], 64);
            out[64] = 0;
        }
}

static void test_concurrent(void)
{
    blk_id_t id = md_attach("ext2-1k.img");
    char want0[65], want1[65];

    set_manifest("ext2-1k.manifest");
    manifest_sha("/sizes/size-12289", want0);
    manifest_sha("/data.bin", want1);
    if (!id || ext2_open(id, &shared) != EXT2_OK) {
        expect(0, "reader for the concurrency test");
        return;
    }
    blk_invalidate(id);
    md.yield = 1;                       /* every device read yields: the two readers interleave */
    rdone = rbad = 0;
    int ok = thread_create("ext2-r0", conc_reader, (void *)0) > 0 && thread_create("ext2-r1", conc_reader, (void *)1) > 0;
    for (unsigned i = 0; ok && rdone < 2 && i < 100000; i++)
        thread_yield();
    md.yield = 0;
    expect(ok && rdone == 2 && rbad == 0 && want0[0] && streq(got[0], want0) && streq(got[1], want1),
           "two threads read two files of one reader at once (device reads yield): both hashes exact");
    thread_yield();
    expect(ext2_close(shared) == EXT2_OK && blk_unregister(id) == BLK_OK && ext2_audit() == NULL,
           "reader idle and closed afterwards");
}

/* ---- the virtio-blk device ---- */

static struct process *const P = selftest_procs;

static int launch(struct process *p, const char *mod)
{
    const struct boot_module *m = module(mod);
    struct elf_image im;
    int eerr;

    if (!m || load_elf(p, mod, phys_to_virt(m->start), m->end - m->start, &im, &eerr) != PROC_OK)
        return 0;
    if (proc_start(p) == PROC_OK)
        return 1;
    proc_destroy(p);
    return 0;
}

static struct ext2_fs *walk_fs;
static volatile unsigned walk_done, walk_bad, walk_checked;

static void walker(void *arg)
{
    unsigned checked = 0;

    (void)arg;
    walk_bad = check_manifest(walk_fs, &checked);
    walk_checked = checked;
    walk_done = 1;
}

static void test_virtio(void)
{
    const struct vblk_info *vi = vblk_info();
    struct ext2_fs *fs;
    struct ext2_stat st;
    unsigned checked = 0, bad = 1;
    uint32_t ino;

    expect(vi->state == VBLK_READY && vi->blk_id, "virtio-blk device ready (the 1 KiB image)");
    if (!vi->blk_id || ext2_open(vi->blk_id, &fs) != EXT2_OK) {
        expect(0, "ext2 reader on the virtio-blk device");
        return;
    }
    set_manifest("ext2-1k.manifest");
    blk_invalidate(vi->blk_id);

    /*
     * The whole manifest through virtio-blk by a kernel thread that yields
     * after every line, while three user programs run.
     */
    proc_test_tick_budget(5000);
    int ok = launch(&P[0], "m-memtag1") && launch(&P[1], "m-memtag2") && launch(&P[2], "m-irqregs");
    walk_fs = fs;
    walk_done = 0;
    walk_yield = 1;
    ok &= thread_create("ext2-walk", walker, 0) > 0;
    for (unsigned w = 0; ok && !walk_done && w < 3000; w += 5)
        thread_sleep(5);
    walk_yield = 0;
    bad = walk_bad;
    checked = walk_checked;
    uint64_t ticks0 = P[0].ticks + P[1].ticks + P[2].ticks;
    for (unsigned i = 0; i < 3; i++)
        proc_kill(&P[i], "test kill");
    unsigned killed = 0;
    for (unsigned i = 0; i < 3; i++) {
        killed += proc_wait(&P[i]) == PROC_KILLED;
        proc_destroy(&P[i]);
    }
    proc_test_tick_budget(PROC_TEST_TICK_BUDGET);
    serial_puts("ext2: virtio-blk: ");
    serial_dec(checked);
    serial_puts(" manifest lines checked, ");
    serial_dec(bad);
    serial_puts(" mismatches\r\n");
    expect(bad == 0 && checked > 200, "the 1 KiB image read through virtio-blk matches its manifest");
    expect(ok && walk_done && killed == 3 && ticks0 >= 6,
           "three user programs ran (and were preempted) while a kernel thread walked the file system");

    expect(ext2_resolve(fs, 0, "/hello.txt", &ino) == EXT2_OK && vblk_reinit() == VBLK_READY &&
           ext2_stat(fs, ino, &st) == EXT2_ESTALE && ext2_close(fs) == EXT2_OK,
           "virtio-blk reinitialized: the reader is stale and closes");
    expect(ext2_open(vi->blk_id, &fs) == EXT2_OK && vblk_shutdown() == VBLK_OK && ext2_stat(fs, ino, &st) ==
           EXT2_ESTALE && ext2_close(fs) == EXT2_OK, "virtio-blk shut down while a reader is open: stale, closes");
    expect(vblk_reinit() == VBLK_READY && ext2_open(vi->blk_id, &fs) == EXT2_OK && ext2_stat(fs, ino, &st) == EXT2_OK &&
           st.size == 32 && ext2_close(fs) == EXT2_OK, "after reinitialization a new reader works");
    expect(ext2_open_count() == 0 && ext2_audit() == NULL && vblk_audit() == NULL && blk_audit() == NULL &&
           !sched_audit() && !proc_audit(), "all audits clean");
}
#endif /* VOS_TEST_CASE_ext2 */

int selftest_ext2(const struct boot_info *bi)
{
    struct ext2_fs *fs = (struct ext2_fs *)1;

    failures = cases = 0;
    mbi = bi;
    test_sha();
    expect(ext2_open(0, &fs) == EXT2_EINVAL && fs == NULL && ext2_open(0xFFFFFFF0u, &fs) == EXT2_EINVAL &&
           ext2_close(NULL) == EXT2_EINVAL && ext2_open_count() == 0 && ext2_audit() == NULL,
           "no reader on an invalid device; audit clean");
#ifdef VOS_TEST_CASE_ext2
    uint64_t f0 = pmm_free_count();
    unsigned dev0 = blk_count();

    test_image("ext2-1k.img", "ext2-1k.manifest");
    test_image("ext2-2k.img", "ext2-2k.manifest");
    test_image("ext2-4k.img", "ext2-4k.manifest");
    test_image("ext2-htree.img", "ext2-htree.manifest");
    test_image("ext2-nofeat.img", "ext2-nofeat.manifest");
    test_image("ext2-empty.img", "ext2-empty.manifest");
    test_errors();
    test_corruption();
    test_lifecycle();
    test_concurrent();
    expect(pmm_free_count() == f0 && blk_count() == dev0 && ext2_open_count() == 0,
           "module-device tests left no frame, device or reader behind");
    test_virtio();
    serial_puts("ext2: ");
    serial_dec(corruptions_run);
    serial_puts(" corrupted or failing images refused for the intended reason\r\n");
#endif
    serial_puts("selftest: ext2 ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}
