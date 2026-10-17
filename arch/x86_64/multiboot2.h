/* Multiboot2 boot information structures (subset used by VOS). */
#ifndef VOS_MULTIBOOT2_H
#define VOS_MULTIBOOT2_H

#include <stdint.h>

#define MB2_BOOTLOADER_MAGIC    0x36D76289u
#define MB2_TAG_ALIGN           8

#define MB2_TAG_END             0
#define MB2_TAG_LOADER_NAME     2
#define MB2_TAG_MMAP            6

#define MB2_MMAP_AVAILABLE      1

/* Fixed header at the start of the boot information block. */
struct mb2_info {
    uint32_t total_size;        /* bytes, including this header */
    uint32_t reserved;
};

struct mb2_tag {
    uint32_t type;
    uint32_t size;              /* bytes, including this header; tags are 8-byte aligned */
};

struct mb2_tag_string {
    struct mb2_tag tag;
    char string[];              /* NUL-terminated */
};

struct mb2_tag_mmap {
    struct mb2_tag tag;
    uint32_t entry_size;
    uint32_t entry_version;
    /* entries of entry_size bytes follow */
};

struct mb2_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

#endif
