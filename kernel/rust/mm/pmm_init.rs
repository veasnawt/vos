//! R4-M3: `pmm_init_in` (arch/x86_64/pmm.c) — instance initialization and the classification of the firmware memory map —
//! as safe, panic-free Rust. The adapter (arch/x86_64/rust/pmm_init.rs) keeps the pmm.h C ABI and is the only unsafe code;
//! the accepted C is frozen as tests/reference/pmm-r4m3.c. C keeps the bitmap storage, the kernel instance and its
//! wrappers, zeroing, poisoning and physical memory.
//!
//! This is a transliteration. Every access the C makes to the instance, the two bitmaps, the boot information, the map
//! entries and the reservation table is one call on [`Mem`], issued in the C's order, and a value is read again wherever a
//! write could have changed it (the loop bounds, the entry pointer's fields after `ignored_bytes` is updated). The core
//! therefore holds no copy of C memory across a write, and any overlap between those objects that is defined in the C has
//! the C's result. The one simplification is that a run of frames is set or cleared one bitmap word at a time instead of
//! one bit at a time: nothing is read between the bit operations of one run, so the stored words are the same.
//!
//! Where the C would leave defined behavior the adapter cannot serve the access and the core stops, with the instance
//! still `FAILED` (it is only marked `READY` by the last write): a missing entry pointer (`EMMAP`), a reservation index
//! past the table (`EINVAL`), a bitmap word outside the caller's storage or an instance whose `avail` field no longer
//! names the caller's bitmap when a protected range is cleared (`ECORRUPT`).
#![forbid(unsafe_code)]

pub const PAGE_SIZE: u64 = 0x1000;
pub const LOW_LIMIT: u64 = 0x10_0000;
pub const MAX_FRAMES: u64 = 262_144;
/// `MB2_MMAP_AVAILABLE` and `sizeof(struct mb2_mmap_entry)` (multiboot2.h).
pub const AVAILABLE: u32 = 1;
pub const ENTRY_MIN: u32 = 24;

pub const READY: i32 = 1;
pub const FAILED: i32 = 2;

pub const OK: i32 = 0;
pub const EMMAP: i32 = 7;
pub const EINVAL: i32 = 8;
pub const ECORRUPT: i32 = 9;

/// The C's memory, one access per call. `Entry` is the adapter's handle for one map entry (the pointer the C keeps in `e`).
pub trait Mem {
    type Entry: Copy;
    /// `memset(p, 0, sizeof(*p))`.
    fn zero_instance(&self);
    fn set_state(&self, v: i32);
    /// `p->avail = avail; p->alloc = alloc;`
    fn set_bitmaps(&self);
    fn set_frames(&self, frames: u64);
    /// `p->avail`, read again from the instance, is still the caller's `avail`.
    fn instance_avail_is_bitmap(&self) -> bool;
    fn ignored(&self) -> u64;
    fn set_ignored(&self, v: u64);
    fn usable(&self) -> u64;
    fn set_usable(&self, v: u64);
    fn set_free(&self, v: u64);
    fn set_hint(&self, v: u64);
    /// Word `w` of the caller's `avail` / `alloc` bitmap; `None` outside its `frames / 64` words.
    fn avail(&self, w: u64) -> Option<u64>;
    fn set_avail(&self, w: u64, v: u64) -> Option<()>;
    fn set_alloc(&self, w: u64, v: u64) -> Option<()>;
    /// `bi->mmap != NULL`, `bi->mmap_entries`, and the two tag fields (`None` if `bi->mmap` is NULL).
    fn mmap_present(&self) -> bool;
    fn mmap_entries(&self) -> u32;
    fn entry_version(&self) -> Option<u32>;
    fn entry_size(&self) -> Option<u32>;
    /// `boot_mmap_entry(bi, i)`; `None` for a NULL result.
    fn entry(&self, i: u32) -> Option<Self::Entry>;
    fn base(&self, e: Self::Entry) -> u64;
    fn length(&self, e: Self::Entry) -> u64;
    fn kind(&self, e: Self::Entry) -> u32;
    /// `t->count` (`None` if `t` is NULL) and `(t->r[i].start, t->r[i].end)` (`None` past the table).
    fn resv_count(&self) -> Option<u64>;
    fn resv(&self, i: u64) -> Option<(u64, u64)>;
}

/// `count_bits`: the accepted C's clear-lowest-bit loop (no popcnt: the build targets baseline x86_64).
fn count_bits(mut x: u64) -> u64 {
    let mut n = 0u64;
    while x != 0 {
        x &= x.wrapping_sub(1);
        n = n.wrapping_add(1);
    }
    n
}

/// The bits of bitmap word `w` whose frames lie in `[first, end)`; the caller only asks for words the run touches.
fn run_mask(first: u64, end: u64, w: u64) -> u64 {
    let low = w.wrapping_mul(64);
    let from = if first > low { first.wrapping_sub(low) } else { 0 };
    let below = if from >= 64 { !0 } else { (1u64 << (from % 64)).wrapping_sub(1) };
    let upto = if end.wrapping_sub(low) >= 64 { !0 } else { (1u64 << (end.wrapping_sub(low) % 64)).wrapping_sub(1) };
    upto & !below
}

/// `set_frames(avail, first, end)` (`set`) or `clear_frames(avail, first, end)`, a word at a time. Inlined, like `protect`:
/// out of line the two cost more stack than the C used.
#[inline(always)]
fn change_frames<M: Mem>(m: &M, first: u64, end: u64, set: bool) -> Option<()> {
    if first >= end {
        return Some(());
    }
    let mut w = first / 64;
    while w.wrapping_mul(64) < end {
        let mask = run_mask(first, end, w);
        let word = m.avail(w)?;
        m.set_avail(w, if set { word | mask } else { word & !mask })?;
        w = w.wrapping_add(1);
    }
    Some(())
}

/// `protect`: remove `[start, end)` from the allocatable set, rounded outward to frames and clipped to the tracked range.
#[inline(always)]
fn protect<M: Mem>(m: &M, start: u64, end: u64, limit: u64) -> Option<()> {
    if start >= limit || start >= end {
        return Some(());
    }
    let e = if end >= limit { limit } else { end.wrapping_add(PAGE_SIZE - 1) & !(PAGE_SIZE - 1) };
    if !m.instance_avail_is_bitmap() {
        return None; // the C would clear through whatever `p->avail` now holds
    }
    change_frames(m, start / PAGE_SIZE, e / PAGE_SIZE, false)
}

/// `pmm_init_in`. `bitmaps_unusable`: `avail` or `alloc` is NULL (or not a usable `uint64_t` pointer); `no_info`: `bi` is NULL.
pub fn init<M: Mem>(m: &M, bitmaps_unusable: bool, frames: u64, no_info: bool) -> i32 {
    m.zero_instance();
    m.set_state(FAILED);
    if bitmaps_unusable || frames == 0 || frames % 64 != 0 || frames > MAX_FRAMES {
        return EINVAL;
    }
    if no_info || !m.mmap_present() || m.mmap_entries() == 0 {
        return EMMAP;
    }
    if m.entry_version() != Some(0) {
        return EMMAP;
    }
    match m.entry_size() {
        Some(size) if size >= ENTRY_MIN && size % 8 == 0 => {}
        _ => return EMMAP,
    }

    let limit = frames.wrapping_mul(PAGE_SIZE);
    let words = frames / 64;

    // 1. Every frame starts unavailable and unallocated.
    m.set_bitmaps();
    m.set_frames(frames);
    let mut w = 0u64;
    while w < words {
        if m.set_avail(w, 0).is_none() {
            return ECORRUPT;
        }
        w = w.wrapping_add(1);
    }
    w = 0;
    while w < words {
        if m.set_alloc(w, 0).is_none() {
            return ECORRUPT;
        }
        w = w.wrapping_add(1);
    }

    // 2. Free pass: whole frames inside available entries, within [1 MiB, limit).
    let mut i = 0u32;
    while i < m.mmap_entries() {
        let Some(e) = m.entry(i) else { return EMMAP };
        i = i.wrapping_add(1);
        let base = m.base(e);
        let length = m.length(e);
        let mut end = base.wrapping_add(length);
        if m.kind(e) != AVAILABLE || length == 0 || end < base {
            continue; // not RAM, empty, or overflowing: never free
        }
        if end > limit {
            let from = if base > limit { base } else { limit };
            m.set_ignored(m.ignored().wrapping_add(end.wrapping_sub(from)));
        }
        let base = m.base(e); // read again: the C reads `e->base` after the update above
        let mut s = if base < LOW_LIMIT { LOW_LIMIT } else { base };
        if end > limit {
            end = limit;
        }
        if s >= end {
            continue;
        }
        s = s.wrapping_add(PAGE_SIZE - 1) & !(PAGE_SIZE - 1); // inward: s < limit, no overflow
        end &= !(PAGE_SIZE - 1);
        if change_frames(m, s / PAGE_SIZE, end / PAGE_SIZE, true).is_none() {
            return ECORRUPT;
        }
    }

    // 3. Protect pass: every non-available entry, of any type, wins.
    i = 0;
    while i < m.mmap_entries() {
        let Some(e) = m.entry(i) else { return EMMAP };
        i = i.wrapping_add(1);
        let base = m.base(e);
        let length = m.length(e);
        let mut end = base.wrapping_add(length);
        if m.kind(e) == AVAILABLE || length == 0 {
            continue;
        }
        if end < base {
            end = u64::MAX; // overflowing: extends to 2^64
        }
        if protect(m, base & !(PAGE_SIZE - 1), end, limit).is_none() {
            return ECORRUPT;
        }
    }

    // 4. Reservation pass (the table is a union; overlaps are harmless).
    let mut r = 0u64;
    while let Some(count) = m.resv_count() {
        if r >= count {
            break;
        }
        let Some((start, end)) = m.resv(r) else { return EINVAL };
        r = r.wrapping_add(1);
        if protect(m, start & !(PAGE_SIZE - 1), end, limit).is_none() {
            return ECORRUPT;
        }
    }

    // 5. Defensive: nothing below 1 MiB, whatever the inputs said. Only the tracked frames: an instance smaller than 1 MiB
    //    has `frames / 64` words (R4-M1H).
    let low = if limit < LOW_LIMIT { limit } else { LOW_LIMIT };
    if change_frames(m, 0, low / PAGE_SIZE, false).is_none() {
        return ECORRUPT;
    }

    w = 0;
    while w < words {
        let Some(word) = m.avail(w) else { return ECORRUPT };
        m.set_usable(m.usable().wrapping_add(count_bits(word)));
        w = w.wrapping_add(1);
    }
    m.set_free(m.usable());
    m.set_hint(0);
    m.set_state(READY);
    OK
}
