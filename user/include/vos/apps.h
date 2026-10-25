/* ABI v4: bounded catalog enumeration and non-interactive run-and-join. */
#ifndef VOS_APPS_H
#define VOS_APPS_H
#include <stdint.h>
#include <stddef.h>
struct vos_app_entry_v1 {
    uint32_t version, size;
    uint64_t app_id, next_cookie;
    uint32_t name_length, flags;
    uint8_t name[16];
};
_Static_assert(sizeof(struct vos_app_entry_v1) == 48, "app entry size");
_Static_assert(offsetof(struct vos_app_entry_v1, version) == 0, "version offset");
_Static_assert(offsetof(struct vos_app_entry_v1, size) == 4, "size offset");
_Static_assert(offsetof(struct vos_app_entry_v1, app_id) == 8, "id offset");
_Static_assert(offsetof(struct vos_app_entry_v1, next_cookie) == 16, "cookie offset");
_Static_assert(offsetof(struct vos_app_entry_v1, name_length) == 24, "length offset");
_Static_assert(offsetof(struct vos_app_entry_v1, flags) == 28, "flags offset");
_Static_assert(offsetof(struct vos_app_entry_v1, name) == 32, "name offset");
#define VOS_APP_ENTRY_VERSION 1
#define VOS_APP_ENTRY_SIZE 48
#define VOS_APP_EXITED 0
#define VOS_APP_FAULTED 1
#define VOS_APP_CANCELLED 2
#define VOS_APP_CANCEL_KERNEL 1
int64_t vos_app_next(uint64_t cookie, struct vos_app_entry_v1 *out, uint64_t capacity);
int64_t vos_app_run_foreground(uint64_t app_id, uint64_t flags);
#endif
