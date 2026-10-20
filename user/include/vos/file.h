/* ABI v2 only: link abi_note_v2.o and syscall_v2.o. */
#ifndef VOS_FILE_H
#define VOS_FILE_H
#include <vos/syscall.h>
#define VOS_SYS_FILE_OPEN_AT 6
#define VOS_SYS_FILE_READ_AT 7
#define VOS_SYS_FILE_STAT 8
#define VOS_SYS_DIRECTORY_NEXT 9
#define VOS_SYS_FILE_INITIAL 10
#define VOS_ENOENT 10
#define VOS_ENOTDIR 11
#define VOS_EISDIR 12
#define VOS_ENAMETOOLONG 13
#define VOS_ELOOP 14
#define VOS_EIO 15
#define VOS_EUNAVAIL 16
#define VOS_ESTALE 17
#define VOS_ETIMEDOUT 18
#define VOS_EINTR 19
#define VOS_EUNSUPP 20
#define VOS_FILE_READ 1
#define VOS_FILE_STAT 2
#define VOS_FILE_ENUMERATE 4
#define VOS_FILE_TRAVERSE 8
#define VOS_FILE_RIGHTS 15
#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stddef.h>
struct vos_file_stat {
    uint64_t size, blocks512;
    uint32_t kind, mode, uid, gid, links, rights;
    uint8_t reserved[24];
};
struct vos_directory_entry {
    uint64_t next_cookie;
    uint32_t kind, name_length;
    uint8_t name[256];
};
_Static_assert(sizeof(struct vos_file_stat) == 64, "stat wire size");
_Static_assert(sizeof(struct vos_directory_entry) == 272, "entry wire size");
_Static_assert(offsetof(struct vos_file_stat, rights) == 36, "rights wire offset");
_Static_assert(offsetof(struct vos_file_stat, reserved) == 40, "reserved wire offset");
_Static_assert(offsetof(struct vos_directory_entry, kind) == 8, "kind wire offset");
_Static_assert(offsetof(struct vos_directory_entry, name) == 16, "name wire offset");
long vos_file_initial(uint64_t index);
long vos_file_open_at(long directory, const void *path, uint64_t length, uint64_t rights);
long vos_file_read_at(long file, void *destination, uint64_t length, uint64_t offset);
long vos_file_stat(long handle, void *destination, uint64_t size);
long vos_directory_next(long directory, uint64_t cookie, void *destination, uint64_t size);
#endif
#endif
