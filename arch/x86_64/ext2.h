/*
 * Read-only ext2 reader (N3.3, storage.md layer 4), on the block layer.
 *
 * The disk is untrusted input. Every on-disk value is read with explicit
 * little-endian loads (never by casting disk bytes to a structure),
 * range-checked with overflow-safe arithmetic before it is used, and every
 * walk has a fixed bound. Nothing is ever written.
 *
 * Supported profile (anything else is refused with EXT2_EUNSUPP, never
 * guessed at):
 *   - revision 1 (dynamic), magic 0xEF53, state exactly "clean" (1);
 *   - block size 1024, 2048 or 4096; fragment (cluster) size = block size;
 *   - inode size 128 or 256;
 *   - compat features: ext_attr, resize_inode, dir_index (directories with
 *     an htree index are read linearly; the index is ignored);
 *   - incompat features: filetype only;
 *   - ro_compat features: sparse_super, large_file;
 *   - at most EXT2_MAX_GROUPS block groups, block bitmap, inode bitmap and
 *     inode table inside their own group (no flex_bg);
 *   - files mapped by the classic direct/indirect/double/triple block map,
 *     holes read as zeros; no extents, inline data, compression or
 *     encryption (such inodes are refused);
 *   - regular files, directories (linear, at most EXT2_MAX_DIR_BLOCKS
 *     blocks, no holes), symbolic links (fast and slow; their target can be
 *     read, but path resolution never follows them), and FIFO, socket and
 *     device inodes (listed and described, not readable).
 *
 * Ownership and lifetime. A reader is one of EXT2_MAX_FS static slots; it
 * owns no frames: its block and sector buffers are part of the slot, and
 * all data reaches it through blk_read into those buffers, never as a
 * pointer into the block cache. Callers receive copies in their own
 * buffers. A reader is bound to its device's block-layer ID and epoch:
 * once the device is reset, reinitialized or unregistered, every operation
 * returns EXT2_ESTALE until it is closed (it never silently reads a
 * possibly different medium). Operations on one reader are serialized (a
 * thread that finds the reader busy waits on its wait queue); different
 * readers are independent.
 */
#ifndef VOS_X86_64_EXT2_H
#define VOS_X86_64_EXT2_H

#include <stddef.h>
#include <stdint.h>

#include "blockdev.h"

#define EXT2_MAX_FS             4
#define EXT2_MAX_GROUPS         256
#define EXT2_NAME_MAX           255
#define EXT2_PATH_MAX           1024    /* bytes in a path */
#define EXT2_MAX_COMPONENTS     64      /* components in a path, "." and ".." included */
#define EXT2_MAX_DIR_BLOCKS     4096
#define EXT2_ROOT_INO           2

#define EXT2_OK             0
#define EXT2_EINVAL         1   /* bad argument: pointer, buffer, inode number, empty path component */
#define EXT2_ENOTEXT2       2   /* no ext2 superblock (magic) */
#define EXT2_EUNSUPP        3   /* outside the supported profile (revision, sizes, features, state, inode flags) */
#define EXT2_ECORRUPT       4   /* inconsistent or out-of-range metadata (ext2_problem() says what) */
#define EXT2_ENOENT         5
#define EXT2_ENOTDIR        6
#define EXT2_EISDIR         7   /* ext2_read on a directory */
#define EXT2_ENOTREG        8   /* ext2_read on something else than a regular file; readlink on a non-link */
#define EXT2_ENAMETOOLONG   9   /* component above 255 bytes, path above EXT2_PATH_MAX, or link target above the buffer */
#define EXT2_ELOOP          10  /* a symbolic link inside a path: links are never followed */
#define EXT2_ETOODEEP       11  /* more than EXT2_MAX_COMPONENTS components */
#define EXT2_EIO            12  /* the device failed a read */
#define EXT2_EUNAVAIL       13  /* the device cannot serve requests now */
#define EXT2_ESTALE         14  /* the device was reset, reinitialized or removed since open: close the reader */
#define EXT2_EBUSY          15  /* no free reader slot, or the reader is in use */
#define EXT2_EOD            16  /* ext2_readdir: no more entries (not an error) */
#define EXT2_EINTR          17  /* N3.4: the calling thread was interrupted while it waited */

/* File types: the directory-entry codes. */
#define EXT2_T_UNKNOWN  0
#define EXT2_T_REG      1
#define EXT2_T_DIR      2
#define EXT2_T_CHR      3
#define EXT2_T_BLK      4
#define EXT2_T_FIFO     5
#define EXT2_T_SOCK     6
#define EXT2_T_LNK      7

struct ext2_fs;

struct ext2_info {
    uint32_t block_size, inode_size, groups, inodes, inodes_per_group, blocks_per_group, first_data_block;
    uint32_t first_ino, reserved_gdt_blocks, feature_compat, feature_incompat, feature_ro_compat;
    uint64_t blocks;
    char label[17];
};

struct ext2_stat {
    uint32_t ino;
    uint16_t mode;          /* type and permission bits as on disk */
    uint8_t type;           /* EXT2_T_* */
    uint16_t links;
    uint32_t uid, gid, flags;
    uint64_t size, blocks512;
};

struct ext2_dirent {
    uint32_t ino;
    uint8_t type;           /* from the entry (EXT2_T_UNKNOWN without the filetype feature) */
    uint8_t name_len;
    char name[EXT2_NAME_MAX + 1];   /* NUL-terminated copy */
};

/* Open a reader on block device dev: superblock, all group descriptors and the root inode are validated. */
int ext2_open(blk_id_t dev, struct ext2_fs **fs);
/* Release the reader (EXT2_EBUSY while an operation runs on it). */
int ext2_close(struct ext2_fs *fs);
int ext2_info(struct ext2_fs *fs, struct ext2_info *info);
int ext2_stat(struct ext2_fs *fs, uint32_t ino, struct ext2_stat *st);
/* One name (1-255 bytes, no '/') in directory dir; "." and ".." included. */
int ext2_lookup(struct ext2_fs *fs, uint32_t dir, const char *name, size_t len, uint32_t *ino);
/*
 * A NUL-terminated path: absolute from the root if it starts with '/',
 * else relative to directory base. Components are separated by single
 * slashes (no empty components); "." and ".." are honoured (".." of the
 * root is the root); symbolic links are not followed (EXT2_ELOOP inside a
 * path; the link itself if it is the last component).
 */
int ext2_resolve(struct ext2_fs *fs, uint32_t base, const char *path, uint32_t *ino);
/* Next used entry of directory dir after *cookie (start with 0); EXT2_EOD at the end. */
int ext2_readdir(struct ext2_fs *fs, uint32_t dir, uint64_t *cookie, struct ext2_dirent *ent);
/* Up to len bytes of regular file ino from offset off into kernel buffer buf; *done = bytes read (0 at EOF). */
int ext2_read(struct ext2_fs *fs, uint32_t ino, uint64_t off, void *buf, uint64_t len, uint64_t *done);
/* The target of symbolic link ino (not NUL-terminated); *len = its length. */
int ext2_readlink(struct ext2_fs *fs, uint32_t ino, char *buf, size_t cap, size_t *len);

const char *ext2_strerror(int err);
/* What the last refused open or failed operation found (diagnostic only; shared by all threads). */
const char *ext2_problem(void);
unsigned ext2_open_count(void);
/* Consistency of the reader slots; NULL if consistent. */
const char *ext2_audit(void);

#endif
