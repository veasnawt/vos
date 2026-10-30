//! Validation of VOS ABI v0 executables (static ELF64 x86_64): the Rust owner of the rules that arch/x86_64/elf.c implemented.
//!
//! A line-for-line port of the C validator, which stays byte-identical in the tree as the frozen differential reference
//! (tests/host/elf-differential.c compares the two, exact result codes and every image field). Pure: it allocates nothing,
//! reads only inside the slice it is given and never follows section headers. Every 64-bit field it reads must be below 2^40
//! before any sum is formed, so no sum overflows; arithmetic is still written so that it could not panic, and the kernel
//! link check forbids Rust panic and formatting machinery. A read outside the slice (never reached: every read is preceded by
//! the bounds test that the C code makes) yields 0 instead of panicking.
#![forbid(unsafe_code)]

pub const ELF_MAX_FILE: u64 = 1 << 20;
pub const ELF_MAX_LOADS: usize = 4;
pub const ELF_MAX_PHNUM: u64 = 8;
pub const ELF_MAX_IMAGE_PAGES: u64 = 256;
pub const ELF_ABI_MAX: u64 = 5;

const PF_X: u32 = 1;
const PF_W: u32 = 2;
const PF_R: u32 = 4;
const PAGE: u64 = 0x1000;
const USER_START: u64 = 0x40_0000;
const USER_END: u64 = 0x4000_0000;
const FIELD_LIMIT: u64 = 1 << 40;
const ET_EXEC: u64 = 2;
const ET_DYN: u64 = 3;
const EM_X86_64: u64 = 62;
const PT_LOAD: u32 = 1;
const PT_NOTE: u32 = 4;
const PT_GNU_STACK: u32 = 0x6474_e551;
const VOS_NOTE_ABI: u64 = 1;

pub const ELF_OK: i32 = 0;
pub const ELF_ESIZE: i32 = 1;
pub const ELF_EMAGIC: i32 = 2;
pub const ELF_EIDENT: i32 = 3;
pub const ELF_EDYN: i32 = 4;
pub const ELF_ETYPE: i32 = 5;
pub const ELF_EMACHINE: i32 = 6;
pub const ELF_EHEADER: i32 = 7;
pub const ELF_EPHBOUNDS: i32 = 8;
pub const ELF_ERANGE: i32 = 9;
pub const ELF_ELOADCOUNT: i32 = 10;
pub const ELF_EALIGN: i32 = 11;
pub const ELF_ESEGSIZE: i32 = 12;
pub const ELF_EUSERRANGE: i32 = 13;
pub const ELF_EFLAGS: i32 = 14;
pub const ELF_EWX: i32 = 15;
pub const ELF_EOVERLAP: i32 = 16;
pub const ELF_ENOEXEC: i32 = 17;
pub const ELF_EIMAGE: i32 = 18;
pub const ELF_EENTRY: i32 = 19;
pub const ELF_ENOTE: i32 = 20;
pub const ELF_ENOTEMISSING: i32 = 21;
pub const ELF_ENOTEDUP: i32 = 22;
pub const ELF_EABI: i32 = 23;
pub const ELF_ESTACK: i32 = 24;
pub const ELF_EPTYPE: i32 = 25;
pub const ELF_NERRORS: usize = 26;

/// `struct elf_segment` of elf.h.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Segment {
    pub vaddr: u64,
    pub memsz: u64,
    pub offset: u64,
    pub filesz: u64,
    pub flags: u32,
}

/// `struct elf_image` of elf.h.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Image {
    pub entry: u64,
    pub nload: u32,
    pub load: [Segment; ELF_MAX_LOADS],
    pub pages: u64,
    pub abi: u32,
}

/// Messages by result code, NUL-terminated, exactly the strings of elf.c's `elf_strerror`.
static NAMES: [&[u8]; ELF_NERRORS] = [
    b"valid\0",
    b"file smaller than an ELF header or larger than 1 MiB\0",
    b"bad ELF magic\0",
    b"unsupported e_ident (class, data, version, OS/ABI or padding)\0",
    b"ET_DYN (PIE or shared object); VOS requires ET_EXEC\0",
    b"e_type is not ET_EXEC\0",
    b"e_machine is not EM_X86_64\0",
    b"unsupported ELF header field\0",
    b"program-header table outside the file\0",
    b"64-bit field out of range\0",
    b"not 1-4 PT_LOAD segments\0",
    b"PT_LOAD not page-aligned\0",
    b"PT_LOAD sizes or file range invalid\0",
    b"PT_LOAD outside [0x400000, 0x40000000)\0",
    b"PT_LOAD flags unknown or not readable\0",
    b"PT_LOAD writable and executable\0",
    b"PT_LOAD unsorted or overlapping\0",
    b"no executable PT_LOAD\0",
    b"image needs more than 1 MiB\0",
    b"entry not in the file-backed part of an executable PT_LOAD\0",
    b"malformed PT_NOTE\0",
    b"no VOS ABI note\0",
    b"more than one VOS ABI note\0",
    b"VOS ABI note: bad descriptor or unsupported version\0",
    b"executable stack requested\0",
    b"unsupported program-header type\0",
];

/// The message for a result code, NUL-terminated; "?" for any code outside 0..26.
pub fn strerror(err: i32) -> &'static [u8] {
    match usize::try_from(err).ok().and_then(|i| NAMES.get(i)) {
        Some(name) => name,
        None => b"?\0",
    }
}

struct Bytes<'a>(&'a [u8]);

impl Bytes<'_> {
    /// Little-endian read of `n` bytes at `off`; 0 outside the slice (unreachable after the bounds tests).
    fn le(&self, off: u64, n: usize) -> u64 {
        let end = usize::try_from(off).ok().and_then(|o| o.checked_add(n).map(|e| (o, e)));
        match end.and_then(|(o, e)| self.0.get(o..e)) {
            Some(s) => s.iter().rev().fold(0u64, |v, &b| v << 8 | u64::from(b)),
            None => 0,
        }
    }

    fn byte(&self, off: u64) -> u8 {
        usize::try_from(off).ok().and_then(|o| self.0.get(o)).copied().unwrap_or(0)
    }
}

/// `off <= size && len <= size - off`, the same predicate as vos_rust_elf_inside.
fn inside(size: u64, off: u64, len: u64) -> bool {
    off <= size && len <= size - off
}

fn check(img: &[u8]) -> Result<Image, i32> {
    let size = img.len() as u64;
    let b = Bytes(img);
    let mut r = Image::default();
    let (mut exec_loads, mut vos_notes, mut entry_ok) = (0u32, 0u32, false);
    let mut prev_end = 0u64;

    if !(64..=ELF_MAX_FILE).contains(&size) {
        return Err(ELF_ESIZE);
    }
    if b.byte(0) != 0x7F || b.byte(1) != b'E' || b.byte(2) != b'L' || b.byte(3) != b'F' {
        return Err(ELF_EMAGIC);
    }
    if b.byte(4) != 2 || b.byte(5) != 1 || b.byte(6) != 1 || b.byte(7) != 0 || b.byte(8) != 0 {
        return Err(ELF_EIDENT);
    }
    for i in 9..16 {
        if b.byte(i) != 0 {
            return Err(ELF_EIDENT);
        }
    }
    let etype = b.le(16, 2);
    if etype == ET_DYN {
        return Err(ELF_EDYN);
    }
    if etype != ET_EXEC {
        return Err(ELF_ETYPE);
    }
    if b.le(18, 2) != EM_X86_64 {
        return Err(ELF_EMACHINE);
    }
    if b.le(20, 4) != 1 {
        return Err(ELF_EHEADER);
    }
    let (entry, phoff) = (b.le(24, 8), b.le(32, 8));
    if entry >= FIELD_LIMIT || phoff >= FIELD_LIMIT {
        return Err(ELF_ERANGE);
    }
    if b.le(48, 4) != 0 || b.le(52, 2) != 64 || b.le(54, 2) != 56 {
        return Err(ELF_EHEADER);
    }
    let phnum = b.le(56, 2);
    if phnum < 1 || phnum > ELF_MAX_PHNUM || phoff < 64 || phoff % 8 != 0 {
        return Err(ELF_EHEADER);
    }
    if !inside(size, phoff, phnum * 56) {
        return Err(ELF_EPHBOUNDS);
    }

    for i in 0..phnum {
        let ph = phoff + i * 56;
        let ptype = b.le(ph, 4) as u32;
        let flags = b.le(ph + 4, 4) as u32;
        let (off, vaddr) = (b.le(ph + 8, 8), b.le(ph + 16, 8));
        let (filesz, memsz) = (b.le(ph + 32, 8), b.le(ph + 40, 8));
        let align = b.le(ph + 48, 8);

        if off >= FIELD_LIMIT || vaddr >= FIELD_LIMIT || filesz >= FIELD_LIMIT || memsz >= FIELD_LIMIT || align >= FIELD_LIMIT {
            return Err(ELF_ERANGE);
        }
        // From here on every sum of two fields is below 2^41: no overflow.

        match ptype {
            PT_LOAD => {
                if r.nload as usize == ELF_MAX_LOADS {
                    return Err(ELF_ELOADCOUNT);
                }
                if align != PAGE || vaddr % PAGE != 0 || off % PAGE != 0 {
                    return Err(ELF_EALIGN);
                }
                if memsz == 0 || filesz > memsz || off + filesz > size {
                    return Err(ELF_ESEGSIZE);
                }
                if vaddr < USER_START || vaddr + memsz > USER_END {
                    return Err(ELF_EUSERRANGE);
                }
                if flags & !(PF_R | PF_W | PF_X) != 0 || flags & PF_R == 0 {
                    return Err(ELF_EFLAGS);
                }
                if flags & PF_W != 0 && flags & PF_X != 0 {
                    return Err(ELF_EWX);
                }
                if vaddr < prev_end {
                    return Err(ELF_EOVERLAP);
                }
                prev_end = (vaddr + memsz + PAGE - 1) & !(PAGE - 1);
                r.pages += (memsz + PAGE - 1) / PAGE;
                if flags & PF_X != 0 {
                    exec_loads += 1;
                    if entry >= vaddr && entry < vaddr + filesz {
                        entry_ok = true;
                    }
                }
                if let Some(slot) = r.load.get_mut(r.nload as usize) {
                    *slot = Segment { vaddr, memsz, offset: off, filesz, flags };
                    r.nload += 1;
                }
            }
            PT_NOTE => {
                if align != 4 || off + filesz > size {
                    return Err(ELF_ENOTE);
                }
                let (mut n, end) = (off, off + filesz);
                while end - n >= 12 {
                    let (namesz, descsz, ntype) = (b.le(n, 4), b.le(n + 4, 4), b.le(n + 8, 4));
                    let (na, da) = ((namesz + 3) & !3u64, (descsz + 3) & !3u64);

                    if na + da > end - n - 12 {
                        return Err(ELF_ENOTE);
                    }
                    if namesz == 4
                        && b.byte(n + 12) == b'V'
                        && b.byte(n + 13) == b'O'
                        && b.byte(n + 14) == b'S'
                        && b.byte(n + 15) == 0
                        && ntype == VOS_NOTE_ABI
                    {
                        if descsz != 4 || b.le(n + 16, 4) > ELF_ABI_MAX {
                            return Err(ELF_EABI);
                        }
                        r.abi = b.le(n + 16, 4) as u32;
                        vos_notes += 1;
                        if vos_notes > 1 {
                            return Err(ELF_ENOTEDUP);
                        }
                    }
                    n += 12 + na + da;
                }
                if n != end {
                    return Err(ELF_ENOTE);
                }
            }
            PT_GNU_STACK => {
                if flags & PF_X != 0 {
                    return Err(ELF_ESTACK);
                }
            }
            _ => return Err(ELF_EPTYPE), // PT_NULL, INTERP, DYNAMIC, TLS, PHDR, ...
        }
    }
    if r.nload == 0 {
        return Err(ELF_ELOADCOUNT);
    }
    if exec_loads == 0 {
        return Err(ELF_ENOEXEC);
    }
    if r.pages > ELF_MAX_IMAGE_PAGES {
        return Err(ELF_EIMAGE);
    }
    if !entry_ok {
        return Err(ELF_EENTRY);
    }
    if vos_notes != 1 {
        return Err(ELF_ENOTEMISSING);
    }
    r.entry = entry;
    Ok(r)
}

/// Validate `img`. The image is all zero on every failure and filled in only on `ELF_OK`, like elf.c's `*out`.
pub fn validate(img: &[u8]) -> (i32, Image) {
    match check(img) {
        Ok(image) => (ELF_OK, image),
        Err(code) => (code, Image::default()),
    }
}
