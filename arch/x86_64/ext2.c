#include "ext2.h"
#include "io_scope.h"
#include "lib.h"
#include "memlayout.h"
#include "thread.h"
#include "vmm.h"

/* Superblock (at byte 1024) field offsets. */
#define SB_INODES       0x00
#define SB_BLOCKS       0x04
#define SB_FIRST_DATA   0x14
#define SB_LOG_BSIZE    0x18
#define SB_LOG_CSIZE    0x1C
#define SB_BPG          0x20
#define SB_CPG          0x24
#define SB_IPG          0x28
#define SB_MAGIC        0x38
#define SB_STATE        0x3A
#define SB_REV          0x4C
#define SB_FIRST_INO    0x54
#define SB_INODE_SIZE   0x58
#define SB_COMPAT       0x5C
#define SB_INCOMPAT     0x60
#define SB_RO_COMPAT    0x64
#define SB_LABEL        0x78
#define SB_RESERVED_GDT 0xCE

#define EXT2_MAGIC          0xEF53
#define STATE_CLEAN         1
#define COMPAT_EXT_ATTR     0x0008
#define COMPAT_RESIZE_INODE 0x0010
#define COMPAT_DIR_INDEX    0x0020
#define INCOMPAT_FILETYPE   0x0002
#define RO_SPARSE_SUPER     0x0001
#define RO_LARGE_FILE       0x0002
#define COMPAT_OK       (COMPAT_EXT_ATTR | COMPAT_RESIZE_INODE | COMPAT_DIR_INDEX)
#define INCOMPAT_OK     INCOMPAT_FILETYPE
#define RO_OK           (RO_SPARSE_SUPER | RO_LARGE_FILE)

/* Group descriptor (32 bytes). */
#define GD_SIZE         32
#define GD_BLOCK_BITMAP 0x00
#define GD_INODE_BITMAP 0x04
#define GD_INODE_TABLE  0x08

/* Inode field offsets. */
#define I_MODE          0x00
#define I_UID           0x02
#define I_SIZE          0x04
#define I_GID           0x18
#define I_LINKS         0x1A
#define I_BLOCKS        0x1C
#define I_FLAGS         0x20
#define I_BLOCK         0x28
#define I_FILE_ACL      0x68
#define I_SIZE_HIGH     0x6C
#define N_BLOCKS        15

/* Inode flags that change how data is stored: compression, encryption, extents, inline data, ... */
#define FL_DATA_INTERPRETATION  (0x4u | 0x100u | 0x200u | 0x400u | 0x800u | 0x40000u | 0x80000u | 0x200000u | \
                                 0x10000000u)

#define GOOD_OLD_FIRST_INO  11
#define FS_WAIT_LIMIT       1024

struct group {
    uint32_t block_bitmap, inode_bitmap, inode_table;
};

struct ext2_fs {
    int used, busy, dead;
    blk_id_t dev;
    uint64_t epoch, dev_sectors, blocks;
    uint32_t bs, spb, ptrs, isz, ipg, bpg, fdb, first_ino, inodes, groups, gdt_blocks, reserved_gdt, itb;
    uint32_t compat, incompat, ro;
    int filetype, large_file, sparse_super;
    char label[17];
    struct group g[EXT2_MAX_GROUPS];
    struct wait_queue wq;
    uint8_t blk[4096] __attribute__((aligned(16)));     /* one block (metadata, directories, file data) */
    uint8_t sect[BLK_SECTOR_SIZE] __attribute__((aligned(16)));    /* one sector (inodes, pointers, bitmaps) */
};

struct inode {
    uint32_t ino;
    uint16_t mode, links;
    uint8_t type;
    uint32_t uid, gid, flags, blocks512, file_acl;
    uint64_t size;
    uint32_t block[N_BLOCKS];
};

static struct ext2_fs fss[EXT2_MAX_FS];
static const char *problem = "none";

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Nonzero if byte c occurs in the n bytes at p. */
static int has_byte(const void *p, uint8_t c, size_t n)
{
    const uint8_t *b = p;

    for (size_t i = 0; i < n; i++)
        if (b[i] == c)
            return 1;
    return 0;
}

static int fail(int err, const char *why)
{
    problem = why;
    return err;
}

static int corrupt(const char *why)
{
    return fail(EXT2_ECORRUPT, why);
}

const char *ext2_problem(void)
{
    return problem;
}

const char *ext2_strerror(int err)
{
    static const char *const s[] = {
        "ok", "invalid argument", "not an ext2 file system", "outside the supported ext2 profile",
        "corrupt file system", "no such entry", "not a directory", "is a directory", "not a regular file",
        "name too long", "symbolic link in path", "path too deep", "device I/O error", "device unavailable",
        "device changed since open", "busy", "end of directory", "interrupted",
    };

    return err >= 0 && err <= EXT2_EINTR ? s[err] : "unknown error";
}

/* ---- device access ---- */

static int dev_read(struct ext2_fs *fs, uint64_t sector, uint32_t count, void *buf, uint64_t cap)
{
    struct blk_desc d;
    if (io_scope_logical())
        return fail(EXT2_EINTR, "storage operation stopped");
    int err = blk_read(fs->dev, sector, count, buf, cap);

    if (io_scope_check())
        return fail(EXT2_EINTR, "storage operation stopped");

    if (err == BLK_ENODEV || blk_describe(fs->dev, &d) != BLK_OK || d.epoch != fs->epoch) {
        fs->dead = 1;
        return fail(EXT2_ESTALE, "the device was reset or removed");
    }
    switch (err) {
    case BLK_OK:        return EXT2_OK;
    case BLK_EUNAVAIL:  return fail(EXT2_EUNAVAIL, "device unavailable");
    case BLK_EBUSY:     return fail(EXT2_EBUSY, "block layer busy");
    case BLK_EINTR:     return fail(EXT2_EINTR, "interrupted while waiting for the device");
    default:            return fail(EXT2_EIO, "device read failed");
    }
}

/* Whole block b (already validated) into fs->blk. */
static int read_block(struct ext2_fs *fs, uint32_t b)
{
    if (b >= fs->blocks)
        return corrupt("block beyond the file system");
    return dev_read(fs, (uint64_t)b * fs->spb, fs->spb, fs->blk, sizeof(fs->blk));
}

/* The sector holding byte off of block b into fs->sect; *at = offset of that byte in fs->sect. */
static int read_sector_of(struct ext2_fs *fs, uint32_t b, uint32_t off, uint32_t *at)
{
    if (b >= fs->blocks || off >= fs->bs)
        return corrupt("block beyond the file system");
    *at = off % BLK_SECTOR_SIZE;
    return dev_read(fs, (uint64_t)b * fs->spb + off / BLK_SECTOR_SIZE, 1, fs->sect, sizeof(fs->sect));
}

/* ---- geometry ---- */

static int is_power(uint32_t n, uint32_t base)
{
    while (n > 1 && n % base == 0)
        n /= base;
    return n == 1;
}

/* Does group g hold a superblock and group-descriptor copy? */
static int has_super(const struct ext2_fs *fs, uint32_t g)
{
    return g <= 1 || !fs->sparse_super || is_power(g, 3) || is_power(g, 5) || is_power(g, 7);
}

static uint32_t group_start(const struct ext2_fs *fs, uint32_t g)
{
    return fs->fdb + g * fs->bpg;
}

static uint32_t group_end(const struct ext2_fs *fs, uint32_t g)
{
    uint64_t e = (uint64_t)group_start(fs, g) + fs->bpg;
    return e > fs->blocks ? (uint32_t)fs->blocks : (uint32_t)e;
}

/* Blocks a file may never use: superblock and descriptor copies, reserved GDT, bitmaps, inode tables. */
static int is_metadata(const struct ext2_fs *fs, uint32_t b)
{
    uint32_t g = (b - fs->fdb) / fs->bpg, gs = group_start(fs, g);
    const struct group *gr = &fs->g[g];

    if (has_super(fs, g) && b - gs < 1 + fs->gdt_blocks + fs->reserved_gdt)
        return 1;
    return b == gr->block_bitmap || b == gr->inode_bitmap ||
           (b >= gr->inode_table && b - gr->inode_table < fs->itb);
}

static int data_block_ok(const struct ext2_fs *fs, uint32_t b)
{
    return b >= fs->fdb && b < fs->blocks && !is_metadata(fs, b);
}

/* Largest file the block map can describe, in bytes. */
static uint64_t map_capacity(const struct ext2_fs *fs)
{
    uint64_t p = fs->ptrs;
    return (12 + p + p * p + p * p * p) * fs->bs;
}

/* ---- inodes ---- */

static int inode_type(uint16_t mode)
{
    switch (mode & 0xF000) {
    case 0x8000: return EXT2_T_REG;
    case 0x4000: return EXT2_T_DIR;
    case 0xA000: return EXT2_T_LNK;
    case 0x1000: return EXT2_T_FIFO;
    case 0x2000: return EXT2_T_CHR;
    case 0x6000: return EXT2_T_BLK;
    case 0xC000: return EXT2_T_SOCK;
    default:     return -1;
    }
}

/* Load and validate inode ino (1..inodes, checked by the caller). */
static int inode_load(struct ext2_fs *fs, uint32_t ino, struct inode *in)
{
    uint32_t g = (ino - 1) / fs->ipg, idx = (ino - 1) % fs->ipg, at;
    uint64_t byte = (uint64_t)idx * fs->isz;
    int err, t;

    /* Allocation: the inode bitmap must mark it in use. */
    if ((err = read_sector_of(fs, fs->g[g].inode_bitmap, idx / 8, &at)))
        return err;
    if (!(fs->sect[at] & (1u << (idx % 8))))
        return corrupt("inode not marked in use in its bitmap");

    if ((err = read_sector_of(fs, fs->g[g].inode_table + (uint32_t)(byte / fs->bs), (uint32_t)(byte % fs->bs), &at)))
        return err;
    const uint8_t *p = fs->sect + at;          /* an inode never crosses a sector: 128 or 256 bytes, aligned */
    memset(in, 0, sizeof(*in));
    in->ino = ino;
    in->mode = le16(p + I_MODE);
    in->uid = le16(p + I_UID);
    in->gid = le16(p + I_GID);
    in->links = le16(p + I_LINKS);
    in->blocks512 = le32(p + I_BLOCKS);
    in->flags = le32(p + I_FLAGS);
    in->file_acl = le32(p + I_FILE_ACL);
    for (int i = 0; i < N_BLOCKS; i++)
        in->block[i] = le32(p + I_BLOCK + 4 * i);
    uint32_t size_hi = le32(p + I_SIZE_HIGH);
    in->size = le32(p + I_SIZE);

    if ((t = inode_type(in->mode)) < 0)
        return corrupt("inode with an invalid file type");
    in->type = (uint8_t)t;
    if (in->links == 0)
        return corrupt("inode in use without links");
    if (in->flags & FL_DATA_INTERPRETATION)
        return fail(EXT2_EUNSUPP, "inode uses extents, inline data, compression or encryption");
    if (in->blocks512 % fs->spb)
        return corrupt("inode block count not a multiple of the block size");
    if (in->type == EXT2_T_REG) {
        if (size_hi && !fs->large_file)
            return corrupt("file size above 4 GiB without the large_file feature");
        in->size |= (uint64_t)size_hi << 32;
        if (in->size > map_capacity(fs))
            return corrupt("file size beyond what the block map can address");
    } else if (in->type == EXT2_T_DIR) {
        if (size_hi)
            return corrupt("directory with a size above 4 GiB");
        if (in->size == 0 || in->size % fs->bs)
            return corrupt("directory size not a positive multiple of the block size");
        if (in->size / fs->bs > EXT2_MAX_DIR_BLOCKS)
            return fail(EXT2_EUNSUPP, "directory larger than the reader's limit");
    } else if (in->type == EXT2_T_LNK) {
        if (in->size == 0 || in->size >= fs->bs)
            return corrupt("symbolic link with an impossible target length");
    }
    return EXT2_OK;
}

/*
 * Physical block of logical block lb of inode in (0 = hole). Every pointer,
 * indirect blocks included, must name a data block inside the file system;
 * the map has at most three levels, so no reference can loop.
 */
static int bmap(struct ext2_fs *fs, const struct inode *in, uint64_t lb, uint32_t *pb)
{
    uint64_t p = fs->ptrs, span = 1;
    uint32_t b;
    int levels, err;

    if (lb < 12) {
        b = in->block[lb];
        levels = 0;
    } else if ((lb -= 12) < p) {
        b = in->block[12];
        levels = 1;
    } else if ((lb -= p) < p * p) {
        b = in->block[13];
        levels = 2;
        span = p;
    } else if ((lb -= p * p) < p * p * p) {
        b = in->block[14];
        levels = 3;
        span = p * p;
    } else {
        return corrupt("block beyond the block map");
    }
    for (; levels > 0; levels--, span /= p) {
        uint32_t at;

        if (b == 0)
            break;                      /* a hole at this level: the whole range is a hole */
        if (!data_block_ok(fs, b))
            return corrupt("indirect block outside the data blocks of the file system");
        if ((err = read_sector_of(fs, b, (uint32_t)(lb / span % p) * 4, &at)))
            return err;
        b = le32(fs->sect + at);
    }
    if (b && !data_block_ok(fs, b))
        return corrupt("block pointer outside the data blocks of the file system");
    *pb = b;
    return EXT2_OK;
}

/* ---- directories ---- */

/* Validate every record of directory block fs->blk (block number lb of directory dir). */
static int dir_block_check(struct ext2_fs *fs, uint32_t dir, uint64_t lb)
{
    uint32_t pos = 0, n = 0;

    while (pos < fs->bs) {
        const uint8_t *e = fs->blk + pos;
        uint32_t ino, rec, nl;

        if (n >= fs->bs / 8)                /* every record takes at least 8 bytes */
            return corrupt("directory block with more records than fit");
        if (fs->bs - pos < 8)
            return corrupt("directory record header crosses the block end");
        ino = le32(e);
        rec = le16(e + 4);
        nl = e[6];
        if (rec < 8 || rec % 4 || rec > fs->bs - pos)
            return corrupt("directory record with an invalid length");
        if (!fs->filetype && e[7])
            return corrupt("directory name length above 255");
        if (8 + nl > rec)
            return corrupt("directory name longer than its record");
        if (ino) {
            if (nl == 0)
                return corrupt("directory entry with an empty name");
            if (ino > fs->inodes)
                return corrupt("directory entry names an inode beyond the inode count");
            if (fs->filetype && e[7] > EXT2_T_LNK)
                return corrupt("directory entry with an unknown file type");
            for (uint32_t i = 0; i < nl; i++)
                if (e[8 + i] == '/' || e[8 + i] == 0)
                    return corrupt("directory entry name contains '/' or NUL");
        }
        if (lb == 0 && n == 0 && (ino != dir || nl != 1 || e[8] != '.'))
            return corrupt("directory does not start with \".\"");
        if (lb == 0 && n == 1 && (ino == 0 || nl != 2 || e[8] != '.' || e[9] != '.'))
            return corrupt("directory has no \"..\" as its second entry");
        pos += rec;
        n++;
    }
    return EXT2_OK;
}

/* Read and validate block lb of directory in into fs->blk. */
static int dir_block(struct ext2_fs *fs, const struct inode *in, uint64_t lb)
{
    uint32_t pb;
    int err;

    if ((err = bmap(fs, in, lb, &pb)))
        return err;
    if (pb == 0)
        return corrupt("hole in a directory");
    if ((err = read_block(fs, pb)))
        return err;
    return dir_block_check(fs, in->ino, lb);
}

/* Reserved inodes other than the root may not be reached through a directory. */
static int entry_ino_ok(const struct ext2_fs *fs, uint32_t ino)
{
    return ino == EXT2_ROOT_INO || (ino >= fs->first_ino && ino <= fs->inodes);
}

/* Find name in directory in: *ino and the entry's type. */
static int dir_find(struct ext2_fs *fs, const struct inode *in, const char *name, uint32_t len, uint32_t *ino,
                    uint8_t *type)
{
    for (uint64_t lb = 0; lb < in->size / fs->bs; lb++) {
        int err = dir_block(fs, in, lb);

        if (err)
            return err;
        for (uint32_t pos = 0; pos < fs->bs; pos += le16(fs->blk + pos + 4)) {
            const uint8_t *e = fs->blk + pos;

            if (le32(e) && e[6] == len && memcmp(e + 8, name, len) == 0) {
                *ino = le32(e);
                *type = fs->filetype ? e[7] : EXT2_T_UNKNOWN;
                if (!entry_ino_ok(fs, *ino))
                    return corrupt("directory entry names a reserved inode");
                return EXT2_OK;
            }
        }
    }
    return fail(EXT2_ENOENT, "no such entry");
}

/* ---- reader slots and serialization ---- */

static struct ext2_fs *slot_of(struct ext2_fs *fs)
{
    uintptr_t base = (uintptr_t)fss, p = (uintptr_t)fs;

    if (p < base || p >= base + sizeof(fss) || (p - base) % sizeof(fss[0]) || !fs->used)
        return NULL;
    return fs;
}

/* Take the reader for one operation and check that its device is still the one it was opened on. */
static int enter(struct ext2_fs *fs)
{
    struct blk_desc d;
    unsigned waits = 0;

    memset(&d, 0, sizeof(d));
    if (!slot_of(fs))
        return fail(EXT2_EINVAL, "not an open reader");
    if (io_scope_check())
        return fail(EXT2_EINTR, "storage operation stopped");
    while (fs->busy) {
        if (++waits > FS_WAIT_LIMIT)
            return fail(EXT2_EBUSY, "reader busy");
        if (io_scope_wait(&fs->wq, 0) == THREAD_INTERRUPTED)
            return fail(EXT2_EINTR, "interrupted while waiting for the reader");
        if (!fs->used)
            return fail(EXT2_EINVAL, "reader closed meanwhile");
    }
    fs->busy = 1;
    if (!fs->dead && (blk_describe(fs->dev, &d) != BLK_OK || d.epoch != fs->epoch))
        fs->dead = 1;
    if (fs->dead) {
        fs->busy = 0;
        thread_wake_all(&fs->wq);
        return fail(EXT2_ESTALE, "the device was reset or removed");
    }
    if (d.state != BLK_DEV_READY) {
        fs->busy = 0;
        thread_wake_all(&fs->wq);
        return fail(EXT2_EUNAVAIL, "device unavailable");
    }
    return EXT2_OK;
}

static int leave(struct ext2_fs *fs, int err)
{
    fs->busy = 0;
    thread_wake_all(&fs->wq);
    return io_scope_check() ? EXT2_EINTR : err;
}

/* An inode number given by a caller: the root or a non-reserved inode. */
static int api_ino(struct ext2_fs *fs, uint32_t ino, struct inode *in)
{
    if (!entry_ino_ok(fs, ino))
        return fail(EXT2_EINVAL, "inode number out of range or reserved");
    return inode_load(fs, ino, in);
}

/* Every page of [buf, buf + len) present, writable, supervisor and not a block-cache frame. */
static int buffer_ok(const void *buf, uint64_t len)
{
    uint64_t va = (uint64_t)buf, first = va & ~(PAGE_SIZE - 1), cache[BLK_CACHE_LINES];
    unsigned nc = blk_cache_frames(cache, BLK_CACHE_LINES);

    if (len == 0)
        return 1;
    if (!buf || va < KERNEL_VMA || va + len - 1 < va)
        return 0;
    for (uint64_t p = first; p >= first && p < va + len; p += PAGE_SIZE) {
        struct vmm_mapping m;

        if (vmm_lookup(vmm_kernel_root(), p, &m) != VMM_OK || !(m.flags & VMM_WRITE) || (m.flags & VMM_USER))
            return 0;
        for (unsigned c = 0; c < nc; c++)
            if ((m.pa & ~(PAGE_SIZE - 1)) == cache[c])
                return 0;
    }
    return 1;
}

/* ---- open and close ---- */

static int parse_super(struct ext2_fs *fs)
{
    const uint8_t *s = fs->blk;
    uint64_t span;

    if (le16(s + SB_MAGIC) != EXT2_MAGIC)
        return fail(EXT2_ENOTEXT2, "no ext2 magic number");
    if (le32(s + SB_REV) != 1)
        return fail(EXT2_EUNSUPP, "revision other than 1 (dynamic)");
    if (le32(s + SB_LOG_BSIZE) > 2)
        return fail(EXT2_EUNSUPP, "block size above 4 KiB");
    fs->bs = 1024u << le32(s + SB_LOG_BSIZE);
    fs->spb = fs->bs / BLK_SECTOR_SIZE;
    fs->ptrs = fs->bs / 4;
    fs->compat = le32(s + SB_COMPAT);
    fs->incompat = le32(s + SB_INCOMPAT);
    fs->ro = le32(s + SB_RO_COMPAT);
    if (fs->incompat & ~INCOMPAT_OK)
        return fail(EXT2_EUNSUPP, "incompatible feature outside the profile");
    if (fs->ro & ~RO_OK)
        return fail(EXT2_EUNSUPP, "read-only-compatible feature outside the profile");
    if (fs->compat & ~COMPAT_OK)
        return fail(EXT2_EUNSUPP, "compatible feature outside the profile (e.g. a journal)");
    if (le16(s + SB_STATE) != STATE_CLEAN)
        return fail(EXT2_EUNSUPP, "not cleanly unmounted (errors or orphans recorded)");
    fs->filetype = (fs->incompat & INCOMPAT_FILETYPE) != 0;
    fs->large_file = (fs->ro & RO_LARGE_FILE) != 0;
    fs->sparse_super = (fs->ro & RO_SPARSE_SUPER) != 0;

    fs->inodes = le32(s + SB_INODES);
    fs->blocks = le32(s + SB_BLOCKS);
    fs->fdb = le32(s + SB_FIRST_DATA);
    fs->bpg = le32(s + SB_BPG);
    fs->ipg = le32(s + SB_IPG);
    fs->first_ino = le32(s + SB_FIRST_INO);
    fs->isz = le16(s + SB_INODE_SIZE);
    fs->reserved_gdt = (fs->compat & COMPAT_RESIZE_INODE) ? le16(s + SB_RESERVED_GDT) : 0;
    memcpy(fs->label, s + SB_LABEL, 16);
    fs->label[16] = 0;

    if (fs->isz != 128 && fs->isz != 256)
        return fail(EXT2_EUNSUPP, "inode size other than 128 or 256");
    if (le32(s + SB_LOG_CSIZE) != le32(s + SB_LOG_BSIZE) || le32(s + SB_CPG) != fs->bpg)
        return corrupt("fragment size differs from the block size");
    if (fs->fdb != (fs->bs == 1024 ? 1u : 0u))
        return corrupt("first data block inconsistent with the block size");
    if (fs->bpg == 0 || fs->bpg % 8 || fs->bpg > 8 * fs->bs)
        return corrupt("invalid blocks per group");
    if (fs->ipg == 0 || fs->ipg > 8 * fs->bs)
        return corrupt("invalid inodes per group");
    if (fs->blocks <= fs->fdb)
        return corrupt("file system without data blocks");
    span = fs->blocks - fs->fdb;
    if ((span + fs->bpg - 1) / fs->bpg > EXT2_MAX_GROUPS)
        return fail(EXT2_EUNSUPP, "more block groups than the reader supports");
    fs->groups = (uint32_t)((span + fs->bpg - 1) / fs->bpg);
    if ((uint64_t)fs->groups * fs->ipg != fs->inodes)
        return corrupt("inode count differs from groups x inodes per group");
    if (fs->first_ino < GOOD_OLD_FIRST_INO || fs->first_ino > fs->inodes)
        return corrupt("first non-reserved inode out of range");
    if (fs->blocks > fs->dev_sectors / fs->spb)
        return corrupt("file system larger than the device (truncated image)");
    fs->itb = (uint32_t)(((uint64_t)fs->ipg * fs->isz + fs->bs - 1) / fs->bs);
    fs->gdt_blocks = (fs->groups * GD_SIZE + fs->bs - 1) / fs->bs;
    if ((uint64_t)1 + fs->gdt_blocks + fs->reserved_gdt + 2 + fs->itb > fs->bpg)
        return corrupt("group metadata larger than a group");
    return EXT2_OK;
}

static int parse_groups(struct ext2_fs *fs)
{
    for (uint32_t g = 0; g < fs->groups; g++) {
        uint64_t byte = (uint64_t)g * GD_SIZE;
        uint32_t at, gs = group_start(fs, g), ge = group_end(fs, g), meta;
        int err;

        if ((err = read_sector_of(fs, fs->fdb + 1 + (uint32_t)(byte / fs->bs), (uint32_t)(byte % fs->bs), &at)))
            return err;
        fs->g[g].block_bitmap = le32(fs->sect + at + GD_BLOCK_BITMAP);
        fs->g[g].inode_bitmap = le32(fs->sect + at + GD_INODE_BITMAP);
        fs->g[g].inode_table = le32(fs->sect + at + GD_INODE_TABLE);

        const struct group *gr = &fs->g[g];
        meta = gs + (has_super(fs, g) ? 1 + fs->gdt_blocks + fs->reserved_gdt : 0);
        if (gr->block_bitmap < meta || gr->block_bitmap >= ge || gr->inode_bitmap < meta || gr->inode_bitmap >= ge ||
            gr->inode_table < meta || gr->inode_table >= ge || ge - gr->inode_table < fs->itb)
            return corrupt("group bitmap or inode table outside its group");
        if (gr->block_bitmap == gr->inode_bitmap ||
            gr->block_bitmap - gr->inode_table < fs->itb || gr->inode_bitmap - gr->inode_table < fs->itb)
            return corrupt("group bitmaps overlap each other or the inode table");
    }
    return EXT2_OK;
}

int ext2_open(blk_id_t dev, struct ext2_fs **out)
{
    struct ext2_fs *fs = NULL;
    struct blk_desc d;
    struct inode root;
    int err;

    if (!out)
        return fail(EXT2_EINVAL, "no result pointer");
    *out = NULL;
    switch (blk_describe(dev, &d)) {
    case BLK_OK:
        break;
    default:
        return fail(EXT2_EINVAL, "no such block device");
    }
    if (d.state != BLK_DEV_READY)
        return fail(EXT2_EUNAVAIL, "device unavailable");
    for (unsigned i = 0; i < EXT2_MAX_FS && !fs; i++)
        if (!fss[i].used)
            fs = &fss[i];
    if (!fs)
        return fail(EXT2_EBUSY, "no free reader slot");
    memset(fs, 0, sizeof(*fs));
    fs->used = fs->busy = 1;
    fs->dev = dev;
    fs->epoch = d.epoch;
    fs->dev_sectors = d.sectors;

    if (d.sectors < 4)
        err = corrupt("device too small for a superblock");
    else if (!(err = dev_read(fs, 2, 2, fs->blk, sizeof(fs->blk))) && !(err = parse_super(fs)) &&
             !(err = parse_groups(fs)) && !(err = inode_load(fs, EXT2_ROOT_INO, &root)) && root.type != EXT2_T_DIR)
        err = corrupt("root inode is not a directory");
    if (!err)
        err = dir_block(fs, &root, 0);      /* "." and ".." of the root */
    if (err) {
        memset(fs, 0, sizeof(*fs));         /* the slot is all the reader owns */
        return err;
    }
    fs->busy = 0;
    *out = fs;
    return EXT2_OK;
}

int ext2_close(struct ext2_fs *fs)
{
    if (!slot_of(fs))
        return fail(EXT2_EINVAL, "not an open reader");
    if (fs->busy || fs->wq.head)
        return fail(EXT2_EBUSY, "reader in use");
    memset(fs, 0, sizeof(*fs));
    return EXT2_OK;
}

unsigned ext2_open_count(void)
{
    unsigned n = 0;

    for (unsigned i = 0; i < EXT2_MAX_FS; i++)
        n += fss[i].used;
    return n;
}

int ext2_info(struct ext2_fs *fs, struct ext2_info *info)
{
    int err;

    if (!info)
        return fail(EXT2_EINVAL, "no result pointer");
    if ((err = enter(fs)))
        return err;
    memset(info, 0, sizeof(*info));
    info->block_size = fs->bs;
    info->inode_size = fs->isz;
    info->groups = fs->groups;
    info->inodes = fs->inodes;
    info->inodes_per_group = fs->ipg;
    info->blocks_per_group = fs->bpg;
    info->first_data_block = fs->fdb;
    info->first_ino = fs->first_ino;
    info->reserved_gdt_blocks = fs->reserved_gdt;
    info->feature_compat = fs->compat;
    info->feature_incompat = fs->incompat;
    info->feature_ro_compat = fs->ro;
    info->blocks = fs->blocks;
    memcpy(info->label, fs->label, sizeof(info->label));
    return leave(fs, EXT2_OK);
}

int ext2_stat(struct ext2_fs *fs, uint32_t ino, struct ext2_stat *st)
{
    struct inode in;
    int err;

    if (!st)
        return fail(EXT2_EINVAL, "no result pointer");
    if ((err = enter(fs)))
        return err;
    if ((err = api_ino(fs, ino, &in)))
        return leave(fs, err);
    st->ino = ino;
    st->mode = in.mode;
    st->type = in.type;
    st->links = in.links;
    st->uid = in.uid;
    st->gid = in.gid;
    st->flags = in.flags;
    st->size = in.size;
    st->blocks512 = in.blocks512;
    return leave(fs, EXT2_OK);
}

/* One component in directory dir: the child's inode, checked against the entry's type. */
static int step(struct ext2_fs *fs, uint32_t dir, const char *name, uint32_t len, struct inode *child)
{
    struct inode d;
    uint32_t ino;
    uint8_t type;
    int err;

    if ((err = inode_load(fs, dir, &d)))
        return err;
    if (d.type != EXT2_T_DIR)
        return fail(EXT2_ENOTDIR, "not a directory");
    if ((err = dir_find(fs, &d, name, len, &ino, &type)))
        return err;
    if ((err = inode_load(fs, ino, child)))
        return err;
    if (type != EXT2_T_UNKNOWN && type != child->type)
        return corrupt("directory entry type differs from the inode");
    return EXT2_OK;
}

int ext2_lookup(struct ext2_fs *fs, uint32_t dir, const char *name, size_t len, uint32_t *ino)
{
    struct inode d, child;
    int err;

    if (!name || !ino || len == 0 || has_byte(name, '/', len) || has_byte(name, 0, len))
        return fail(EXT2_EINVAL, "invalid name");
    if (len > EXT2_NAME_MAX)
        return fail(EXT2_ENAMETOOLONG, "name longer than 255 bytes");
    if ((err = enter(fs)))
        return err;
    if (!(err = api_ino(fs, dir, &d)) && !(err = step(fs, dir, name, (uint32_t)len, &child)))
        *ino = child.ino;
    return leave(fs, err);
}

int ext2_resolve(struct ext2_fs *fs, uint32_t base, const char *path, uint32_t *ino)
{
    struct inode cur;
    size_t len = 0;
    unsigned comps = 0;
    int err;

    if (!path || !ino)
        return fail(EXT2_EINVAL, "no path");
    while (len <= EXT2_PATH_MAX && path[len])
        len++;
    if (len == 0)
        return fail(EXT2_EINVAL, "empty path");
    if (len > EXT2_PATH_MAX)
        return fail(EXT2_ENAMETOOLONG, "path longer than the limit");
    if ((err = enter(fs)))
        return err;
    if ((err = api_ino(fs, path[0] == '/' ? EXT2_ROOT_INO : base, &cur)))
        return leave(fs, err);
    if (cur.type != EXT2_T_DIR)
        return leave(fs, fail(EXT2_ENOTDIR, "base is not a directory"));
    size_t i = path[0] == '/' ? 1 : 0;
    if (i == len) {                         /* "/" */
        *ino = cur.ino;
        return leave(fs, EXT2_OK);
    }
    for (;;) {
        size_t start = i;

        while (i < len && path[i] != '/')
            i++;
        if (i == start)
            return leave(fs, fail(EXT2_EINVAL, "empty path component"));
        if (i - start > EXT2_NAME_MAX)
            return leave(fs, fail(EXT2_ENAMETOOLONG, "path component longer than 255 bytes"));
        if (++comps > EXT2_MAX_COMPONENTS)
            return leave(fs, fail(EXT2_ETOODEEP, "path deeper than the limit"));
        if (cur.type == EXT2_T_LNK)
            return leave(fs, fail(EXT2_ELOOP, "symbolic link inside a path (not followed)"));
        if (cur.type != EXT2_T_DIR)
            return leave(fs, fail(EXT2_ENOTDIR, "path component is not a directory"));
        if (!(i - start == 1 && path[start] == '.')) {
            struct inode next;

            if ((err = step(fs, cur.ino, path + start, (uint32_t)(i - start), &next)))
                return leave(fs, err);
            cur = next;
        }
        if (i == len)
            break;
        i++;                                /* the slash */
        if (i == len)
            return leave(fs, fail(EXT2_EINVAL, "empty path component"));
    }
    *ino = cur.ino;
    return leave(fs, EXT2_OK);
}

int ext2_readdir(struct ext2_fs *fs, uint32_t dir, uint64_t *cookie, struct ext2_dirent *ent)
{
    struct inode d;
    int err;

    if (!cookie || !ent)
        return fail(EXT2_EINVAL, "no cookie or entry");
    if ((err = enter(fs)))
        return err;
    if ((err = api_ino(fs, dir, &d)))
        return leave(fs, err);
    if (d.type != EXT2_T_DIR)
        return leave(fs, fail(EXT2_ENOTDIR, "not a directory"));
    while (*cookie < d.size) {
        uint64_t lb = *cookie / fs->bs;
        uint32_t want = (uint32_t)(*cookie % fs->bs), pos = 0;

        if ((err = dir_block(fs, &d, lb)))
            return leave(fs, err);
        while (pos < want)                  /* the cookie must be a record boundary */
            pos += le16(fs->blk + pos + 4);
        if (pos != want)
            return leave(fs, fail(EXT2_EINVAL, "directory cookie is not a record boundary"));
        for (; pos < fs->bs; pos += le16(fs->blk + pos + 4)) {
            const uint8_t *e = fs->blk + pos;

            if (!le32(e))
                continue;
            /* On-disk directory references are metadata, not caller inode
             * arguments. Match lookup's rejection before publishing an entry. */
            if (!entry_ino_ok(fs, le32(e)))
                return leave(fs, corrupt("directory entry names a reserved inode"));
            ent->ino = le32(e);
            ent->type = fs->filetype ? e[7] : EXT2_T_UNKNOWN;
            ent->name_len = e[6];
            memcpy(ent->name, e + 8, e[6]);
            ent->name[e[6]] = 0;
            *cookie = lb * fs->bs + pos + le16(e + 4);
            return leave(fs, EXT2_OK);
        }
        *cookie = (lb + 1) * fs->bs;
    }
    return leave(fs, EXT2_EOD);
}

int ext2_read(struct ext2_fs *fs, uint32_t ino, uint64_t off, void *buf, uint64_t len, uint64_t *done)
{
    struct inode in;
    uint8_t *out = buf;
    int err;

    if (!done)
        return fail(EXT2_EINVAL, "no result pointer");
    *done = 0;
    if (!buffer_ok(buf, len))
        return fail(EXT2_EINVAL, "buffer is not writable kernel memory");
    if ((err = enter(fs)))
        return err;
    if ((err = api_ino(fs, ino, &in)))
        return leave(fs, err);
    if (in.type != EXT2_T_REG)
        return leave(fs, fail(in.type == EXT2_T_DIR ? EXT2_EISDIR : EXT2_ENOTREG, "not a regular file"));
    if (off >= in.size)
        return leave(fs, EXT2_OK);
    if (len > in.size - off)
        len = in.size - off;
    while (*done < len) {
        uint64_t pos = off + *done, lb = pos / fs->bs;
        uint32_t within = (uint32_t)(pos % fs->bs), pb;
        uint64_t chunk = fs->bs - within;

        if (chunk > len - *done)
            chunk = len - *done;
        if ((err = bmap(fs, &in, lb, &pb)))
            return leave(fs, err);
        if (pb == 0) {
            memset(out + *done, 0, chunk);
        } else {
            if ((err = read_block(fs, pb)))
                return leave(fs, err);
            memcpy(out + *done, fs->blk + within, chunk);
        }
        *done += chunk;
    }
    return leave(fs, EXT2_OK);
}

int ext2_readlink(struct ext2_fs *fs, uint32_t ino, char *buf, size_t cap, size_t *len)
{
    struct inode in;
    int err;

    if (!buf || !len)
        return fail(EXT2_EINVAL, "no buffer");
    if ((err = enter(fs)))
        return err;
    if ((err = api_ino(fs, ino, &in)))
        return leave(fs, err);
    if (in.type != EXT2_T_LNK)
        return leave(fs, fail(EXT2_ENOTREG, "not a symbolic link"));
    if (in.size > cap)
        return leave(fs, fail(EXT2_ENAMETOOLONG, "link target longer than the buffer"));
    uint32_t nblocks = in.blocks512 / fs->spb, acl = in.file_acl ? 1 : 0;
    if (in.size < 4 * N_BLOCKS && nblocks <= acl) {
        for (uint32_t i = 0; i < in.size; i++)     /* fast link: the target is stored in i_block */
            buf[i] = (char)(in.block[i / 4] >> (8 * (i % 4)));
    } else {
        uint32_t pb;

        if (nblocks < acl || nblocks - acl != 1)
            return leave(fs, corrupt("slow symbolic link without exactly one data block"));
        if ((err = bmap(fs, &in, 0, &pb)))
            return leave(fs, err);
        if (pb == 0)
            return leave(fs, corrupt("slow symbolic link without a data block"));
        if ((err = read_block(fs, pb)))
            return leave(fs, err);
        memcpy(buf, fs->blk, in.size);
    }
    if (has_byte(buf, 0, in.size))
        return leave(fs, corrupt("symbolic link target contains NUL"));
    *len = in.size;
    return leave(fs, EXT2_OK);
}

const char *ext2_audit(void)
{
    for (unsigned i = 0; i < EXT2_MAX_FS; i++) {
        const struct ext2_fs *fs = &fss[i];

        if (!fs->used) {
            if (fs->busy || fs->dev || fs->wq.head)
                return "free reader slot with state";
            continue;
        }
        if (fs->busy)
            return "reader left busy";
        if ((fs->bs != 1024 && fs->bs != 2048 && fs->bs != 4096) || fs->spb * BLK_SECTOR_SIZE != fs->bs ||
            fs->groups == 0 || fs->groups > EXT2_MAX_GROUPS || (uint64_t)fs->groups * fs->ipg != fs->inodes ||
            fs->blocks > fs->dev_sectors / fs->spb)
            return "reader geometry inconsistent";
    }
    return NULL;
}

#ifdef VOS_TEST
#include "storage_observe.h"

void ext2_test_observe(struct observe_reader out[EXT2_MAX_FS])
{
    for (unsigned i = 0; i < EXT2_MAX_FS; i++) {
        const struct ext2_fs *fs = &fss[i];
        out[i] = (struct observe_reader){(uint64_t)fs, fs->epoch,
            (uint64_t)fs->wq.head, (uint64_t)fs->wq.tail,
            fs->used, fs->busy, fs->dead, fs->dev};
    }
}
#endif
