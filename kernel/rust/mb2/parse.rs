//! A2: the Multiboot2 information-block walk of `mb2_parse`, ported from the C of `arch/x86_64/bootinfo.c` without any change
//! of behavior. Pure: no unsafe code, no allocation, no panic path, no I/O. Every read of the loader's block goes through
//! `get` and returns 0 out of range (unreachable, because each read lies inside a tag the walk has already bounded).
//!
//! The walk never builds the output structure: it reports each decision to an `Out` (the platform glue writes the fields of
//! the C `struct boot_info`; the host tests record them). Offsets are byte offsets into the loader's block; 0 means "not
//! set" (every offset that is set is at least 8). The writes happen at the same points as in the C, so after an error the
//! output holds exactly what the C would have left.

/// Sanity bound for the information block (`MB2_INFO_MAX`).
pub const MB2_INFO_MAX: u32 = 0x10000;
/// The boot page tables map physical [0, 1 GiB) (`BOOT_MAP_LIMIT`).
pub const BOOT_MAP_LIMIT: u64 = 0x4000_0000;
pub const BOOT_MAX_MODULES: u32 = 64;

const INFO_HEADER: usize = 8; // sizeof(struct mb2_info)
const TAG_HEADER: usize = 8; // sizeof(struct mb2_tag)
const MODULE_HEADER: usize = 16; // sizeof(struct mb2_tag_module)
const MMAP_HEADER: usize = 16; // sizeof(struct mb2_tag_mmap)
const MMAP_ENTRY_MIN: u32 = 24; // sizeof(struct mb2_mmap_entry)

const TAG_END: u32 = 0;
const TAG_CMDLINE: u32 = 1;
const TAG_LOADER_NAME: u32 = 2;
const TAG_MODULE: u32 = 3;
const TAG_MMAP: u32 = 6;
const TAG_FRAMEBUFFER: u32 = 8;
const TAG_EFI32_SYSTAB: u32 = 11;
const TAG_EFI64_SYSTAB: u32 = 12;
const TAG_EFI_MMAP: u32 = 17;
const TAG_EFI_BS: u32 = 18;

/// The error strings, byte for byte as in C, each NUL-terminated so a pointer to the first byte is a C string.
pub const E_ADDRESS: &[u8] = b"info block address invalid\0";
pub const E_SIZE: &[u8] = b"info block size invalid\0";
pub const E_TAG_SIZE: &[u8] = b"tag size invalid\0";
pub const E_END_SIZE: &[u8] = b"end tag size invalid\0";
pub const E_NO_MMAP: &[u8] = b"no memory map\0";
pub const E_DUP_CMDLINE: &[u8] = b"duplicate command line tag\0";
pub const E_CMDLINE_TERM: &[u8] = b"command line not terminated\0";
pub const E_MODULE_SMALL: &[u8] = b"module tag too small\0";
pub const E_MODULE_TERM: &[u8] = b"module command line not terminated\0";
pub const E_MODULE_RANGE: &[u8] = b"module range empty or reversed\0";
pub const E_MODULE_HIGH: &[u8] = b"module extends above 1 GiB\0";
pub const E_MODULE_MANY: &[u8] = b"too many modules\0";
pub const E_DUP_MMAP: &[u8] = b"duplicate memory map tag\0";
pub const E_MMAP_INVALID: &[u8] = b"memory map tag invalid\0";
pub const E_MMAP_VERSION: &[u8] = b"memory map version unsupported\0";
pub const E_MMAP_EMPTY: &[u8] = b"memory map empty\0";
pub const E_MMAP_PARTIAL: &[u8] = b"memory map has a partial entry\0";
pub const E_NO_END: &[u8] = b"no end tag\0";

/// Where the walk reports what it found. Offsets are into the loader's block.
pub trait Out {
    /// The block's physical range [start, end).
    fn info(&mut self, start: u64, end: u64);
    fn loader(&mut self, offset: u32);
    fn cmdline(&mut self, offset: u32);
    /// `index` < `BOOT_MAX_MODULES`; `cmdline` is the offset of the module's string.
    fn module(&mut self, index: u32, start: u64, end: u64, cmdline: u32);
    fn module_count(&mut self, count: u32);
    /// The first framebuffer tag.
    fn fb(&mut self, tag: u32);
    fn fb_tags(&mut self, count: u32);
    fn efi(&mut self);
    /// The first EFI memory-map tag.
    fn efi_mmap(&mut self, tag: u32);
    fn efi_mmap_tags(&mut self, count: u32);
    fn efi_bs_active(&mut self);
    /// The memory-map tag and the number of entries it holds.
    fn mmap(&mut self, tag: u32, entries: u32);
}

/// The address checks that come first in the C: non-zero, 8-byte aligned and room for the header below the boot map limit.
pub fn check_address(info_phys: u64) -> Result<(), &'static [u8]> {
    if info_phys == 0 || info_phys % 8 != 0 || info_phys > BOOT_MAP_LIMIT - INFO_HEADER as u64 {
        return Err(E_ADDRESS);
    }
    Ok(())
}

/// The size checks on `total_size`, the first word of the block. `info_phys` has passed `check_address`.
pub fn check_total(total: u32, info_phys: u64) -> Result<(), &'static [u8]> {
    if (total as usize) < INFO_HEADER + TAG_HEADER || total > MB2_INFO_MAX || info_phys + u64::from(total) > BOOT_MAP_LIMIT {
        return Err(E_SIZE);
    }
    Ok(())
}

fn byte(b: &[u8], at: usize) -> u8 {
    b.get(at).copied().unwrap_or(0)
}

fn le32(b: &[u8], at: usize) -> u32 {
    u32::from(byte(b, at)) | u32::from(byte(b, at + 1)) << 8 | u32::from(byte(b, at + 2)) << 16 | u32::from(byte(b, at + 3)) << 24
}

/// True if the string area [at, at + len) is non-empty and ends in NUL.
fn terminated(b: &[u8], at: usize, len: usize) -> bool {
    len > 0 && byte(b, at + len - 1) == 0
}

/// Walk the tags of `block`, which is exactly the `total_size` bytes of the information block (`check_total` has passed).
pub fn walk<O: Out>(block: &[u8], out: &mut O) -> Result<(), &'static [u8]> {
    let total = block.len();
    let mut off = INFO_HEADER;
    let (mut have_cmdline, mut have_mmap) = (false, false);
    let (mut modules, mut fb_tags, mut efi_mmap_tags) = (0u32, 0u32, 0u32);
    while off + TAG_HEADER <= total {
        let ty = le32(block, off);
        let size = le32(block, off + 4) as usize;
        if size < TAG_HEADER || size > total - off {
            return Err(E_TAG_SIZE);
        }
        match ty {
            TAG_END => {
                if size != TAG_HEADER {
                    return Err(E_END_SIZE);
                }
                return if have_mmap { Ok(()) } else { Err(E_NO_MMAP) };
            }
            TAG_CMDLINE => {
                if have_cmdline {
                    return Err(E_DUP_CMDLINE);
                }
                if !terminated(block, off + TAG_HEADER, size - TAG_HEADER) {
                    return Err(E_CMDLINE_TERM);
                }
                out.cmdline((off + TAG_HEADER) as u32);
                have_cmdline = true;
            }
            TAG_LOADER_NAME => {
                if terminated(block, off + TAG_HEADER, size - TAG_HEADER) {
                    out.loader((off + TAG_HEADER) as u32);
                }
            }
            TAG_MODULE => {
                if size < MODULE_HEADER {
                    return Err(E_MODULE_SMALL);
                }
                if !terminated(block, off + MODULE_HEADER, size - MODULE_HEADER) {
                    return Err(E_MODULE_TERM);
                }
                let (start, end) = (le32(block, off + 8), le32(block, off + 12));
                if start >= end {
                    return Err(E_MODULE_RANGE);
                }
                if u64::from(end) > BOOT_MAP_LIMIT {
                    return Err(E_MODULE_HIGH);
                }
                if modules >= BOOT_MAX_MODULES {
                    return Err(E_MODULE_MANY);
                }
                out.module(modules, u64::from(start), u64::from(end), (off + MODULE_HEADER) as u32);
                modules += 1;
                out.module_count(modules);
            }
            TAG_FRAMEBUFFER => {
                if fb_tags == 0 {
                    out.fb(off as u32);
                }
                fb_tags = fb_tags.wrapping_add(1);
                out.fb_tags(fb_tags);
            }
            TAG_EFI32_SYSTAB | TAG_EFI64_SYSTAB => out.efi(),
            TAG_EFI_MMAP => {
                if efi_mmap_tags == 0 {
                    out.efi_mmap(off as u32);
                }
                efi_mmap_tags = efi_mmap_tags.wrapping_add(1);
                out.efi_mmap_tags(efi_mmap_tags);
            }
            TAG_EFI_BS => out.efi_bs_active(),
            TAG_MMAP => {
                if have_mmap {
                    return Err(E_DUP_MMAP);
                }
                // The entry size is only read once the tag is known to hold it (the C evaluates it after the size test).
                let entry_size = if size >= MMAP_HEADER { le32(block, off + 8) } else { 0 };
                if size < MMAP_HEADER || entry_size < MMAP_ENTRY_MIN || entry_size % 8 != 0 {
                    return Err(E_MMAP_INVALID);
                }
                if le32(block, off + 12) != 0 {
                    return Err(E_MMAP_VERSION);
                }
                if size == MMAP_HEADER {
                    return Err(E_MMAP_EMPTY);
                }
                if (size - MMAP_HEADER) % entry_size as usize != 0 {
                    return Err(E_MMAP_PARTIAL);
                }
                out.mmap(off as u32, ((size - MMAP_HEADER) / entry_size as usize) as u32);
                have_mmap = true;
            }
            _ => {}
        }
        off += (size + 7) & !7;
    }
    Err(E_NO_END)
}
