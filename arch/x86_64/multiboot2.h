/* Multiboot2 boot information structures (subset used by VOS). */
#ifndef VOS_MULTIBOOT2_H
#define VOS_MULTIBOOT2_H

#include <stdint.h>

#define MB2_BOOTLOADER_MAGIC    0x36D76289u
#define MB2_TAG_ALIGN           8

#define MB2_TAG_END             0
#define MB2_TAG_CMDLINE         1
#define MB2_TAG_LOADER_NAME     2
#define MB2_TAG_MODULE          3
#define MB2_TAG_MMAP            6
#define MB2_TAG_FRAMEBUFFER     8
#define MB2_TAG_EFI32_SYSTAB    11
#define MB2_TAG_EFI64_SYSTAB    12

#define MB2_FB_TYPE_INDEXED     0
#define MB2_FB_TYPE_RGB         1
#define MB2_FB_TYPE_EGA_TEXT    2

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

struct mb2_tag_module {
    struct mb2_tag tag;
    uint32_t mod_start;         /* physical, inclusive */
    uint32_t mod_end;           /* physical, exclusive */
    char cmdline[];             /* NUL-terminated */
};

struct mb2_tag_mmap {
    struct mb2_tag tag;
    uint32_t entry_size;
    uint32_t entry_version;
    /* entries of entry_size bytes follow */
};

/* Framebuffer information (type 8); for MB2_FB_TYPE_RGB six colour bytes follow. */
struct mb2_tag_framebuffer {
    struct mb2_tag tag;
    uint64_t addr;              /* physical */
    uint32_t pitch;             /* bytes per line */
    uint32_t width, height;     /* pixels (RGB) or characters (EGA text) */
    uint8_t bpp;
    uint8_t type;               /* MB2_FB_TYPE_* */
    uint16_t reserved;
};

struct mb2_fb_rgb {             /* bit position and size of each channel */
    uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
};

struct mb2_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

#endif
