//! USB input bring-up, Increment 2: hosted tests for the read-only firmware facts: the Multiboot2 information block
//! (memory map, ACPI RSDP), ACPI table walking, DMAR and IVRS parsing, remapping-unit registers, and the four DMA-protection
//! facts that must stay distinct (table present, hardware answers, translation enabled, xHCI translated). Negative cases:
//! truncated and hostile blocks, bad checksums, unreadable memory, overlong lengths, more entries than the bounds.
//! The fake memory refuses any read longer than the platform's bound, so the bound is part of what is tested.
#![forbid(unsafe_code)]
use std::collections::BTreeMap;
use vos_xhci::xhci::firmware::*;
use vos_xhci::xhci::survey::{Mmio, Out};

// ---- builders --------------------------------------------------------------------------------------------------------------------

fn fix(mut v: Vec<u8>, sum_at: usize) -> Vec<u8> {
    v[sum_at] = 0;
    let s = v.iter().fold(0u8, |a, &b| a.wrapping_add(b));
    v[sum_at] = 0u8.wrapping_sub(s);
    v
}
fn sdt(sig: &[u8; 4], body: &[u8]) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(sig);
    v.extend_from_slice(&((36 + body.len()) as u32).to_le_bytes());
    v.push(1); // revision
    v.push(0); // checksum
    v.extend_from_slice(b"VOSTST");
    v.extend_from_slice(b"VOSTABLE");
    v.extend_from_slice(&[0; 4]);
    v.extend_from_slice(b"VOS ");
    v.extend_from_slice(&[0; 4]);
    v.extend_from_slice(body);
    fix(v, 9)
}
fn rsdp_bytes(rev: u8, rsdt: u32, xsdt: u64) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(b"RSD PTR ");
    v.push(0);
    v.extend_from_slice(b"VOSTST");
    v.push(rev);
    v.extend_from_slice(&rsdt.to_le_bytes());
    let mut v = fix(v, 8);
    if rev >= 2 {
        v.extend_from_slice(&36u32.to_le_bytes());
        v.extend_from_slice(&xsdt.to_le_bytes());
        v.push(0);
        v.extend_from_slice(&[0; 3]);
        v = fix(v, 32);
    }
    v
}
fn tag(ty: u32, body: &[u8]) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(&ty.to_le_bytes());
    v.extend_from_slice(&((8 + body.len()) as u32).to_le_bytes());
    v.extend_from_slice(body);
    while v.len() % 8 != 0 {
        v.push(0);
    }
    v
}
fn mmap_tag(entries: &[(u64, u64, u32)]) -> Vec<u8> {
    let mut b = Vec::new();
    b.extend_from_slice(&24u32.to_le_bytes());
    b.extend_from_slice(&0u32.to_le_bytes());
    for &(base, len, ty) in entries {
        b.extend_from_slice(&base.to_le_bytes());
        b.extend_from_slice(&len.to_le_bytes());
        b.extend_from_slice(&ty.to_le_bytes());
        b.extend_from_slice(&0u32.to_le_bytes());
    }
    tag(6, &b)
}
fn info(tags: &[Vec<u8>]) -> Vec<u8> {
    let mut body = Vec::new();
    for t in tags {
        body.extend_from_slice(t);
    }
    body.extend_from_slice(&tag(0, &[])[..8]);
    let mut v = Vec::new();
    v.extend_from_slice(&((8 + body.len()) as u32).to_le_bytes());
    v.extend_from_slice(&0u32.to_le_bytes());
    v.extend_from_slice(&body);
    v
}

#[derive(Default)]
struct Mem {
    regions: BTreeMap<u64, Vec<u8>>,
    reads: usize,
    refuse: Vec<u64>,
}
impl Phys for Mem {
    fn read(&mut self, pa: u64, out: &mut [u8]) -> bool {
        assert!(out.len() <= TABLE_LIMIT as usize && !out.is_empty(), "a read of {} bytes exceeds the platform bound", out.len());
        self.reads += 1;
        if self.refuse.contains(&pa) {
            return false;
        }
        for (&base, bytes) in &self.regions {
            if pa >= base && pa.saturating_add(out.len() as u64) <= base + bytes.len() as u64 {
                let o = (pa - base) as usize;
                out.copy_from_slice(&bytes[o..o + out.len()]);
                return true;
            }
        }
        false
    }
}
impl Mem {
    fn put(&mut self, pa: u64, b: Vec<u8>) -> &mut Self {
        self.regions.insert(pa, b);
        self
    }
}

const XHCI: (u8, u8, u8) = (0, 0x14, 0);

fn scope(ty: u8, bus: u8, path: &[(u8, u8)]) -> Vec<u8> {
    let mut v = vec![ty, (6 + 2 * path.len()) as u8, 0, 0, 0, bus];
    for &(d, f) in path {
        v.push(d);
        v.push(f);
    }
    v
}
fn drhd(flags: u8, base: u64, scopes: &[Vec<u8>]) -> Vec<u8> {
    let mut body = vec![flags, 0];
    body.extend_from_slice(&0u16.to_le_bytes());
    body.extend_from_slice(&base.to_le_bytes());
    for s in scopes {
        body.extend_from_slice(s);
    }
    let mut v = 0u16.to_le_bytes().to_vec();
    v.extend_from_slice(&((4 + body.len()) as u16).to_le_bytes());
    v.extend_from_slice(&body);
    v
}
fn rmrr(scopes: &[Vec<u8>]) -> Vec<u8> {
    let mut body = vec![0u8; 4]; // reserved + segment
    body.extend_from_slice(&0x7f00_0000u64.to_le_bytes());
    body.extend_from_slice(&0x7f00_ffffu64.to_le_bytes());
    for s in scopes {
        body.extend_from_slice(s);
    }
    let mut v = 1u16.to_le_bytes().to_vec();
    v.extend_from_slice(&((4 + body.len()) as u16).to_le_bytes());
    v.extend_from_slice(&body);
    v
}
fn dmar(structs: &[Vec<u8>]) -> Vec<u8> {
    let mut body = vec![38u8, 0x01]; // host address width - 1, flags (INTR_REMAP)
    body.extend_from_slice(&[0; 10]);
    for s in structs {
        body.extend_from_slice(s);
    }
    sdt(b"DMAR", &body)
}
fn ivhd(ty: u8, base: u64) -> Vec<u8> {
    let mut v = vec![ty, 0];
    v.extend_from_slice(&24u16.to_le_bytes());
    v.extend_from_slice(&[0; 4]); // devid, cap offset
    v.extend_from_slice(&base.to_le_bytes());
    v.extend_from_slice(&0u16.to_le_bytes()); // segment
    v.extend_from_slice(&[0; 6]);
    v
}
fn ivrs(structs: &[Vec<u8>]) -> Vec<u8> {
    let mut body = vec![0u8; 12]; // IVinfo + reserved
    for s in structs {
        body.extend_from_slice(s);
    }
    sdt(b"IVRS", &body)
}
/// An ACPI world: RSDP revision 2, an XSDT listing `tables`, every table placed at a distinct high address.
fn world(tables: &[(&[u8; 4], Vec<u8>)]) -> (Mem, Rsdp) {
    let mut m = Mem::default();
    let mut ptrs = Vec::new();
    for (i, (_, t)) in tables.iter().enumerate() {
        let pa = 0x7ffe_0000 + 0x4000 * (i as u64 + 1);
        m.put(pa, t.clone());
        ptrs.extend_from_slice(&pa.to_le_bytes());
    }
    let xsdt = sdt(b"XSDT", &ptrs);
    m.put(0x7ffe_0000, xsdt);
    (m, Rsdp { revision: 2, rsdt: 0, xsdt: 0x7ffe_0000 })
}

#[derive(Default)]
struct Capture(Vec<u8>);
impl Out for Capture {
    fn put(&mut self, t: &[u8]) {
        self.0.extend_from_slice(t);
    }
}
impl Capture {
    fn text(&self) -> String {
        String::from_utf8(self.0.clone()).unwrap()
    }
}

// ---- Multiboot2 information block -------------------------------------------------------------------------------------------

#[test]
fn the_memory_map_and_the_rsdp_are_found_in_the_information_block() {
    let blk = info(&[
        tag(1, b"cmdline\0"),
        mmap_tag(&[(0, 0x9fc00, 1), (0x9fc00, 0x400, 2), (0x10_0000, 0x7fe0_0000, 1), (0x7fff_0000, 0x1_0000, 3), (0x7ffe_0000, 0x1_0000, 4), (0xfe00_0000, 0x20_0000, 2)]),
        tag(14, &rsdp_bytes(0, 0x7ffe_1000, 0)),
    ]);
    let (map, rsdp) = parse_info(&blk);
    assert_eq!(map.len(), 6);
    assert_eq!(rsdp, Ok(Rsdp { revision: 0, rsdt: 0x7ffe_1000, xsdt: 0 }));
    for (addr, kind) in [(0x1000, MemKind::Available), (0x9fc00, MemKind::Reserved), (0x7fff_0000, MemKind::AcpiReclaim), (0x7ffe_0000, MemKind::AcpiNvs), (0xfe00_1000, MemKind::Reserved), (0xd000_0000, MemKind::Unlisted)] {
        assert_eq!(map.kind(addr), kind, "{addr:#x}");
    }
    // The new-style RSDP (type 15) wins over the old one (type 14), in either order.
    for order in [[14u32, 15], [15, 14]] {
        let t: Vec<Vec<u8>> = order.iter().map(|&t| tag(t, &rsdp_bytes(if t == 15 { 2 } else { 0 }, 0x1111_0000, 0x2_2222_0000))).collect();
        let (_, r) = parse_info(&info(&t));
        assert_eq!(r.unwrap().xsdt, 0x2_2222_0000, "{order:?}");
    }
}

#[test]
fn a_page_any_ram_entry_claims_is_not_firmware_memory_and_overlaps_never_hide_reserved_memory() {
    let blk = info(&[mmap_tag(&[(0, 0x8000_0000, 1), (0x7ff0_0000, 0x10_0000, 3), (0x7ffe_0000, 0x1000, 2)])]);
    let (map, _) = parse_info(&blk);
    assert_eq!(map.kind(0x7ff0_0000), MemKind::AcpiReclaim, "the non-RAM entry wins over the RAM one");
    assert!(map.firmware_range(0x7ff0_0000, 0x2000) && map.firmware_range(0x7ffe_0000, 1));
    assert!(!map.firmware_range(0x7fef_f000, 0x2000), "starts in RAM");
    assert!(!map.firmware_range(0x1000, 0x1000) && !map.firmware_range(0x9000_0000, 0x1000), "RAM and unlisted memory");
    assert!(!map.firmware_range(0x7ff0_0000, 0) && !map.firmware_range(u64::MAX - 10, 100));
    // a range of firmware memory is accepted up to 1 MiB and refused beyond it, however much firmware memory there is
    let (big, _) = parse_info(&info(&[mmap_tag(&[(0x1_0000_0000, 0x1000_0000, 4)])]));
    assert!(big.firmware_range(0x1_0000_0000, 1 << 20) && !big.firmware_range(0x1_0000_0000, (1 << 20) + 1));
    let (empty, _) = parse_info(&info(&[]));
    assert!(empty.is_empty() && !empty.firmware_range(0x7ff0_0000, 16) && empty.kind(0x1000) == MemKind::Unlisted);
}

#[test]
fn rsdp_defects_are_named() {
    let good = rsdp_bytes(2, 0x1000, 0x2000);
    let case = |f: &dyn Fn(&mut Vec<u8>)| {
        let mut b = good.clone();
        f(&mut b);
        parse_info(&info(&[tag(15, &b)])).1
    };
    assert_eq!(case(&|_| {}), Ok(Rsdp { revision: 2, rsdt: 0x1000, xsdt: 0x2000 }));
    assert_eq!(case(&|b| b[0] = b'X'), Err("RSDP signature wrong"));
    assert_eq!(case(&|b| b[8] ^= 1), Err("RSDP checksum wrong"));
    assert_eq!(case(&|b| b[32] ^= 1), Err("RSDP extended checksum wrong"));
    assert_eq!(case(&|b| b.truncate(12)), Err("RSDP truncated"));
    assert_eq!(case(&|b| b.truncate(30)), Err("RSDP truncated"));
    assert_eq!(parse_info(&info(&[])).1, Err("no ACPI RSDP tag in the boot information"));
    assert_eq!(parse_info(&[1, 2, 3]).1, Err("boot information block too small"));
}

#[test]
fn hostile_information_blocks_never_panic_and_never_read_past_their_end() {
    let good = info(&[mmap_tag(&[(0, 0x1000, 1)]), tag(15, &rsdp_bytes(2, 1, 2))]);
    // every truncation, and every single-byte corruption of a small block
    for n in 0..good.len() {
        let _ = parse_info(&good[..n]);
    }
    for i in 0..good.len() {
        for v in [0u8, 0xff, 0x80, 0x07] {
            let mut b = good.clone();
            b[i] = v;
            let _ = parse_info(&b);
        }
    }
    // tag sizes: below 8, huge, the end tag with a size, a total size larger than the slice
    let mut b = good.clone();
    b[8 + 4..8 + 8].copy_from_slice(&3u32.to_le_bytes());
    let _ = parse_info(&b);
    let mut b = good.clone();
    b[0..4].copy_from_slice(&u32::MAX.to_le_bytes());
    let _ = parse_info(&b);
    // 300 tags: bounded, and the memory map of the first 128 entries is truncated, not extended
    let many: Vec<Vec<u8>> = (0..300).map(|_| tag(1, b"x\0")).collect();
    let _ = parse_info(&info(&many));
    let entries: Vec<(u64, u64, u32)> = (0..200).map(|i| (i * 0x1000, 0x1000, 1)).collect();
    let (map, _) = parse_info(&info(&[mmap_tag(&entries)]));
    assert_eq!((map.len(), map.truncated), (MAP_LIMIT, true));
    // entry sizes that are too small or huge are ignored
    for es in [0u32, 8, 23, 257, u32::MAX] {
        let mut t = mmap_tag(&[(0, 0x1000, 1)]);
        t[8..12].copy_from_slice(&es.to_le_bytes());
        assert!(parse_info(&info(&[t])).0.is_empty(), "{es}");
    }
    // a pseudo-random sweep
    let mut x = 0x2545_f491_4f6c_dd1du64;
    for _ in 0..5000 {
        let n = (x % 300) as usize;
        let mut b = vec![0u8; n];
        for v in b.iter_mut() {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            *v = x as u8;
        }
        if n >= 4 {
            b[0..4].copy_from_slice(&(n as u32).to_le_bytes());
        }
        let _ = parse_info(&b);
    }
}

// ---- ACPI tables -------------------------------------------------------------------------------------------------------------

#[test]
fn the_tables_are_listed_and_dmar_and_ivrs_are_found_by_signature() {
    let (mut m, rsdp) = world(&[(b"FACP", sdt(b"FACP", &[0; 100])), (b"DMAR", dmar(&[])), (b"IVRS", ivrs(&[])), (b"APIC", sdt(b"APIC", &[0; 8]))]);
    let t = walk_acpi(&mut m, &rsdp);
    assert_eq!((t.err, t.count, t.root_is_xsdt, t.more_than_listed), (None, 4, true, false));
    assert_eq!(&t.sigs[..4], &[*b"FACP", *b"DMAR", *b"IVRS", *b"APIC"]);
    assert_eq!((t.dmar, t.ivrs), (Some(0x7ffe_0000 + 0x8000), Some(0x7ffe_0000 + 0xc000)));
    // no IOMMU table: the root was read and checked, so absence is a result
    let (mut m, rsdp) = world(&[(b"FACP", sdt(b"FACP", &[0; 100]))]);
    let t = walk_acpi(&mut m, &rsdp);
    assert_eq!((t.err, t.dmar, t.ivrs), (None, None, None));
    assert_eq!(dma_facts(&t, &[], 0).table_present, Tri::No);
}

#[test]
fn an_rsdt_is_used_when_there_is_no_xsdt_and_the_xsdt_wins_when_there_is_one() {
    let mut m = Mem::default();
    m.put(0x7ffe_1000, dmar(&[]));
    m.put(0x7ffe_0000, sdt(b"RSDT", &0x7ffe_1000u32.to_le_bytes()));
    let t = walk_acpi(&mut m, &Rsdp { revision: 0, rsdt: 0x7ffe_0000, xsdt: 0 });
    assert_eq!((t.err, t.root_is_xsdt, t.dmar), (None, false, Some(0x7ffe_1000)));
    // revision 2 with a zero XSDT falls back to the RSDT; revision 0 ignores a stale XSDT field
    let t = walk_acpi(&mut m, &Rsdp { revision: 2, rsdt: 0x7ffe_0000, xsdt: 0 });
    assert!(!t.root_is_xsdt && t.err.is_none());
    let t = walk_acpi(&mut m, &Rsdp { revision: 0, rsdt: 0x7ffe_0000, xsdt: 0xdead_0000 });
    assert!(!t.root_is_xsdt && t.err.is_none());
}

#[test]
fn a_root_table_that_cannot_be_trusted_makes_absence_unknown_not_false() {
    let (mut m, rsdp) = world(&[(b"DMAR", dmar(&[]))]);
    // unreadable root
    let mut m2 = Mem::default();
    assert_eq!(walk_acpi(&mut m2, &rsdp).err, Some("root table unreadable"));
    // bad checksum, wrong signature, absurd length
    let mut bad = m.regions[&0x7ffe_0000].clone();
    bad[9] ^= 1;
    m.put(0x7ffe_0000, bad);
    assert_eq!(walk_acpi(&mut m, &rsdp).err, Some("root table checksum wrong"));
    let mut bad = sdt(b"XSDT", &0x7ffe_4000u64.to_le_bytes());
    bad[0] = b'Y';
    m.put(0x7ffe_0000, fix(bad, 9));
    assert_eq!(walk_acpi(&mut m, &rsdp).err, Some("root table malformed"));
    let mut bad = sdt(b"XSDT", &0x7ffe_4000u64.to_le_bytes());
    bad[4..8].copy_from_slice(&(ROOT_LIMIT + 8).to_le_bytes());
    m.put(0x7ffe_0000, bad);
    assert_eq!(walk_acpi(&mut m, &rsdp).err, Some("root table malformed"));
    let t = walk_acpi(&mut m, &rsdp);
    assert_eq!(dma_facts(&t, &[], 0).table_present, Tri::Unknown);
    // a listed table that cannot be read is not an absent one
    let (mut m, rsdp) = world(&[(b"FACP", sdt(b"FACP", &[0; 8])), (b"DMAR", dmar(&[]))]);
    m.refuse.push(0x7ffe_0000 + 0x4000);
    let t = walk_acpi(&mut m, &rsdp);
    assert_eq!((t.err, t.count, t.dmar.is_some()), (Some("a listed table is unreadable"), 1, true));
    assert_eq!(dma_facts(&t, &[], 0).table_present, Tri::Yes, "a found DMAR is present even if another table failed");
    let (mut m, rsdp) = world(&[(b"FACP", sdt(b"FACP", &[0; 8]))]);
    m.refuse.push(0x7ffe_0000 + 0x4000);
    assert_eq!(dma_facts(&walk_acpi(&mut m, &rsdp), &[], 0).table_present, Tri::Unknown);
}

#[test]
fn more_entries_than_the_bound_are_cut_and_reported() {
    let mut m = Mem::default();
    let mut ptrs = Vec::new();
    for i in 0..100u64 {
        let pa = 0x8000_0000 + 0x1000 * i;
        m.put(pa, sdt(b"SSDT", &[0; 4]));
        ptrs.extend_from_slice(&pa.to_le_bytes());
    }
    // 120 entries (36 + 960 = 996 <= ROOT_LIMIT); the last 20 repeat the first table
    while ptrs.len() < 8 * 120 {
        ptrs.extend_from_slice(&0x8000_0000u64.to_le_bytes());
    }
    m.put(0x7ffe_0000, sdt(b"XSDT", &ptrs));
    let t = walk_acpi(&mut m, &Rsdp { revision: 2, rsdt: 0, xsdt: 0x7ffe_0000 });
    assert!(t.more_than_listed, "{t:?}");
    assert!(usize::from(t.count) <= SIG_LIMIT && m.reads <= 2 * ENTRY_LIMIT + 16, "reads {}", m.reads);
    assert_eq!(dma_facts(&t, &[], 0).table_present, Tri::Unknown, "a truncated list cannot prove absence");
}

#[test]
fn a_cut_root_list_is_reported_even_when_the_unread_part_adds_no_signatures() {
    // 70 entries: ten readable tables, then entries that point at nothing readable. The list is longer than the bound.
    let mut m = Mem::default();
    let mut ptrs = Vec::new();
    for i in 0..70u64 {
        let pa = 0x8000_0000 + 0x1000 * i;
        if i < 10 {
            m.put(pa, sdt(b"SSDT", &[0; 4]));
        }
        ptrs.extend_from_slice(&pa.to_le_bytes());
    }
    m.put(0x7ffe_0000, sdt(b"XSDT", &ptrs));
    let t = walk_acpi(&mut m, &Rsdp { revision: 2, rsdt: 0, xsdt: 0x7ffe_0000 });
    assert!(t.more_than_listed && t.count == 10, "{t:?}");
    assert_eq!(t.err, Some("a listed table is unreadable"));
}

// ---- DMAR --------------------------------------------------------------------------------------------------------------------

fn dmar_case(structs: &[Vec<u8>]) -> Dmar {
    let mut m = Mem::default();
    m.put(0x7ffe_4000, dmar(structs));
    parse_dmar(&mut m, 0x7ffe_4000, XHCI)
}

#[test]
fn dmar_units_scopes_and_coverage_of_the_xhci_are_resolved_conservatively() {
    let named = scope(1, 0, &[(0x14, 0)]);
    let other = scope(1, 0, &[(0x1a, 0)]);
    let bridge = scope(2, 0, &[(0x1c, 0)]);
    let nested = scope(1, 0, &[(0x1c, 0), (0, 0)]);
    let ioapic = scope(3, 0, &[(0x1f, 7)]);
    let d = dmar_case(&[drhd(0, 0xfed9_0000, &[named.clone()]), drhd(1, 0xfed9_1000, &[])]);
    assert_eq!((d.host_width, d.flags, d.n_units, d.units_total, d.err), (39, 1, 2, 2, None));
    assert_eq!((d.units[0].base, d.units[0].cover), (0xfed9_0000, Cover::Specific));
    assert_eq!(d.units[1].cover, Cover::No, "the all-devices unit does not take a device another unit names");
    // an all-devices unit alone covers everything
    assert_eq!(dmar_case(&[drhd(1, 0xfed9_1000, &[ioapic.clone()])]).units[0].cover, Cover::PciAll);
    // the right device but another function is not the xHCI; a unit that is both named and "all" is named
    assert_eq!(dmar_case(&[drhd(0, 0xfed9_0000, &[scope(1, 0, &[(0x14, 1)])])]).units[0].cover, Cover::No);
    assert_eq!(dmar_case(&[drhd(1, 0xfed9_0000, &[named.clone()])]).units[0].cover, Cover::Specific);
    // other endpoints do not cover the xHCI; bridges and nested paths could, so they are "unknown"
    assert_eq!(dmar_case(&[drhd(0, 0xfed9_0000, &[other.clone(), ioapic.clone()])]).units[0].cover, Cover::No);
    assert_eq!(dmar_case(&[drhd(0, 0xfed9_0000, &[bridge])]).units[0].cover, Cover::Unknown);
    assert_eq!(dmar_case(&[drhd(0, 0xfed9_0000, &[nested])]).units[0].cover, Cover::Unknown);
    // a scope on another bus never names the xHCI
    assert_eq!(dmar_case(&[drhd(0, 0xfed9_0000, &[scope(1, 3, &[(0x14, 0)])])]).units[0].cover, Cover::No);
    // RMRR: memory the BIOS keeps using for a device
    let d = dmar_case(&[drhd(1, 0xfed9_0000, &[]), rmrr(&[named]), rmrr(&[other])]);
    assert_eq!((d.rmrr, d.rmrr_names_xhci), (2, true));
    assert!(!dmar_case(&[rmrr(&[scope(1, 0, &[(0x1a, 0)])])]).rmrr_names_xhci);
}

#[test]
fn dmar_units_beyond_the_bound_are_counted_not_stored() {
    let units: Vec<Vec<u8>> = (0..6).map(|i| drhd(0, 0xfed9_0000 + 0x1000 * i, &[])).collect();
    let d = dmar_case(&units);
    assert_eq!((usize::from(d.n_units), d.units_total), (UNIT_LIMIT, 6));
}

#[test]
fn malformed_dmar_tables_are_refused_by_name() {
    let t = |f: &dyn Fn(&mut Vec<u8>)| {
        let mut b = dmar(&[drhd(0, 0xfed9_0000, &[scope(1, 0, &[(0x14, 0)])])]);
        f(&mut b);
        let mut m = Mem::default();
        m.put(0x7ffe_4000, b);
        parse_dmar(&mut m, 0x7ffe_4000, XHCI).err
    };
    assert_eq!(t(&|_| {}), None);
    assert_eq!(t(&|b| b[0] = b'X'), Some("DMAR malformed"));
    assert_eq!(t(&|b| b[4..8].copy_from_slice(&(TABLE_LIMIT + 1).to_le_bytes())), Some("DMAR malformed"));
    assert_eq!(t(&|b| b[4..8].copy_from_slice(&40u32.to_le_bytes())), Some("DMAR malformed"));
    assert_eq!(t(&|b| b[9] ^= 1), Some("DMAR checksum wrong"));
    // structure lengths: zero, past the table end; the loop must stop, not spin
    for len in [0u16, 1, 3, 0xffff] {
        let r = t(&|b| {
            b[50..52].copy_from_slice(&len.to_le_bytes());
            *b = fix(b.clone(), 9);
        });
        assert_eq!(r, Some("DMAR structure length invalid"), "{len}");
    }
    // a scope with a bad length: the unit is kept, coverage is unknown
    let mut m = Mem::default();
    let mut s = scope(1, 0, &[(0x14, 0)]);
    s[1] = 3;
    m.put(0x7ffe_4000, dmar(&[drhd(0, 0xfed9_0000, &[s])]));
    assert_eq!(parse_dmar(&mut m, 0x7ffe_4000, XHCI).units[0].cover, Cover::Unknown);
    // unreadable
    assert_eq!(parse_dmar(&mut Mem::default(), 0x7ffe_4000, XHCI).err, Some("DMAR unreadable"));
}

#[test]
fn ivrs_blocks_are_counted_and_malformed_ones_refused() {
    let mut m = Mem::default();
    m.put(0x7ffe_8000, ivrs(&[ivhd(0x10, 0xfeb8_0000), ivhd(0x11, 0xfeb9_0000), ivhd(0x40, 0xfeba_0000), vec![0x20, 0, 4, 0], ivhd(0x10, 0xfebb_0000), ivhd(0x10, 0xfebc_0000)]));
    let v = parse_ivrs(&mut m, 0x7ffe_8000);
    assert_eq!((v.err, usize::from(v.n_units), v.units_total, v.ivmd), (None, UNIT_LIMIT, 5, 1));
    assert_eq!(&v.bases[..3], &[0xfeb8_0000, 0xfeb9_0000, 0xfeba_0000]);
    let mut bad = ivrs(&[ivhd(0x10, 0xfeb8_0000)]);
    bad[0] = b'X';
    m.put(0x7ffe_8000, bad);
    assert_eq!(parse_ivrs(&mut m, 0x7ffe_8000).err, Some("IVRS malformed"));
    let mut bad = ivrs(&[ivhd(0x10, 0xfeb8_0000)]);
    bad[9] ^= 1;
    m.put(0x7ffe_8000, bad);
    assert_eq!(parse_ivrs(&mut m, 0x7ffe_8000).err, Some("IVRS checksum wrong"));
    let mut bad = ivrs(&[ivhd(0x10, 0xfeb8_0000)]);
    bad[50..52].copy_from_slice(&0xffffu16.to_le_bytes());
    m.put(0x7ffe_8000, fix(bad, 9));
    assert_eq!(parse_ivrs(&mut m, 0x7ffe_8000).err, Some("IVRS structure length invalid"));
}

// ---- remapping unit registers ------------------------------------------------------------------------------------------------

struct Regs(BTreeMap<u32, u32>, Vec<u32>);
impl Mmio for Regs {
    fn read32(&mut self, off: u32) -> Option<u32> {
        assert!(off % 4 == 0 && off < 0x1000, "unit register read {off:#x} outside the 4 KiB window");
        self.1.push(off);
        self.0.get(&off).copied()
    }
}

#[test]
fn unit_registers_are_decoded_and_the_translation_bit_is_not_confused_with_presence() {
    let intel = |ver: u32, gsts: u32| Regs([(0, ver), (8, 0x00c0_0000), (0xc, 0x0), (0x10, 0xf0_101a), (0x14, 0), (0x1c, gsts)].into(), vec![]);
    let off = read_intel_unit(&mut intel(0x10, 0));
    assert!(off.responds && !off.intel_translation());
    assert_eq!((off.version, off.cap, off.ecap), (0x10, 0x00c0_0000, 0xf0_101a));
    assert!(read_intel_unit(&mut intel(0x10, 1 << 31)).intel_translation(), "TES is bit 31");
    assert!(!read_intel_unit(&mut intel(0x10, 1 << 30)).intel_translation(), "RTPS alone is not translation");
    assert!(!read_intel_unit(&mut intel(0, 1 << 31)).responds && !read_intel_unit(&mut intel(0xffff_ffff, 1 << 31)).intel_translation());
    assert!(!read_intel_unit(&mut Regs(BTreeMap::new(), vec![])).responds, "a refused read is not a response");
    let amd = |ctl: u32| Regs([(0x18, ctl), (0x30, 5), (0x34, 6)].into(), vec![]);
    assert!(read_amd_unit(&mut amd(1)).amd_enabled() && !read_amd_unit(&mut amd(0)).amd_enabled());
    assert!(!read_amd_unit(&mut amd(0xffff_ffff)).responds);
    // exactly the documented registers are read: VER, CAP, ECAP, GSTS and nothing else
    let mut r = intel(0x10, 0);
    read_intel_unit(&mut r);
    assert_eq!(r.1, vec![0, 8, 0xc, 0x10, 0x14, 0x1c]);
}

#[test]
fn unit_windows_are_confined_to_the_chipset_hole() {
    for (base, ok) in [(0xfed9_0000u64, true), (0xfe00_0000, true), (0xfeff_f000, true), (0xfed9_0800, false), (0xfdff_f000, false), (0xff00_0000, false), (0x7ffe_0000, false), (0, false), (u64::MAX & !0xfff, false)] {
        assert_eq!(unit_base_ok(base), ok, "{base:#x}");
    }
}

// ---- the four DMA facts -------------------------------------------------------------------------------------------------------

fn regs(responds: bool, status: u32) -> UnitRegs {
    UnitRegs { responds, version: if responds { 0x10 } else { 0 }, cap: 0, ecap: 0, status }
}
fn tables(dmar: bool, ivrs: bool, err: Option<&'static str>) -> Tables {
    Tables { dmar: dmar.then_some(1), ivrs: ivrs.then_some(1), err, ..Default::default() }
}

#[test]
fn the_four_facts_stay_distinct_and_unknown_is_never_turned_into_no() {
    let tes = 1u32 << 31;
    use Cover::*;
    let f = |t: &Tables, u: &[(Cover, UnitRegs, bool)], found| dma_facts(t, u, found);
    // present but never read: only the first fact is known
    let a = f(&tables(true, false, None), &[], 1);
    assert_eq!((a.table_present, a.hardware_responds, a.translation_enabled, a.xhci_translated), (Tri::Yes, Tri::Unknown, Tri::Unknown, Tri::Unknown));
    // present, answers, translation off, names the xHCI: the xHCI is NOT translated
    let a = f(&tables(true, false, None), &[(Specific, regs(true, 0), false)], 1);
    assert_eq!((a.table_present, a.hardware_responds, a.translation_enabled, a.xhci_translated), (Tri::Yes, Tri::Yes, Tri::No, Tri::No));
    // translation on and covering the xHCI
    let a = f(&tables(true, false, None), &[(Specific, regs(true, tes), false)], 1);
    assert_eq!((a.translation_enabled, a.xhci_translated), (Tri::Yes, Tri::Yes));
    let a = f(&tables(true, false, None), &[(PciAll, regs(true, tes), false)], 1);
    assert_eq!(a.xhci_translated, Tri::Yes);
    // translation on in a unit that does not cover the xHCI: enabled yes, xHCI no
    let a = f(&tables(true, false, None), &[(No, regs(true, tes), false)], 1);
    assert_eq!((a.translation_enabled, a.xhci_translated), (Tri::Yes, Tri::No));
    // translation on, coverage unresolved: the xHCI is unknown, not no
    let a = f(&tables(true, false, None), &[(Unknown, regs(true, tes), false)], 1);
    assert_eq!((a.translation_enabled, a.xhci_translated), (Tri::Yes, Tri::Unknown));
    // a unit that does not answer: hardware no only if every unit was read and none answered; translation unknown
    let a = f(&tables(true, false, None), &[(Specific, regs(false, 0), false)], 1);
    assert_eq!((a.hardware_responds, a.translation_enabled, a.xhci_translated), (Tri::No, Tri::Unknown, Tri::Unknown));
    // three units found, one read and silent: not "no" yet
    let a = f(&tables(true, false, None), &[(Specific, regs(false, 0), false)], 3);
    assert_eq!((a.hardware_responds, a.translation_enabled, a.xhci_translated), (Tri::Unknown, Tri::Unknown, Tri::Unknown));
    // more units found than read: nothing can be concluded to be "no"
    let a = f(&tables(true, false, None), &[(Specific, regs(true, 0), false)], 3);
    assert_eq!((a.hardware_responds, a.translation_enabled, a.xhci_translated), (Tri::Yes, Tri::Unknown, Tri::Unknown));
    // AMD: the control bit, with coverage unknown
    let a = f(&tables(false, true, None), &[(Unknown, regs(true, 1), true)], 1);
    assert_eq!((a.table_present, a.translation_enabled, a.xhci_translated), (Tri::Yes, Tri::Yes, Tri::Unknown));
    let a = f(&tables(false, true, None), &[(Unknown, regs(true, 0), true)], 1);
    assert_eq!((a.translation_enabled, a.xhci_translated), (Tri::No, Tri::Unknown));
    // no table at all: absence is only a result when the root was fully read
    assert_eq!(f(&tables(false, false, None), &[], 0).table_present, Tri::No);
    assert_eq!(f(&tables(false, false, Some("root table unreadable")), &[], 0).table_present, Tri::Unknown);
    assert_eq!(f(&tables(false, false, None), &[], 0).hardware_responds, Tri::Unknown, "no table does not prove no hardware");
}

#[test]
fn the_report_never_claims_isolation() {
    use Cover::*;
    let tes = 1u32 << 31;
    for (t, u, found) in [
        (tables(false, false, None), vec![], 0u8),
        (tables(true, false, None), vec![(Specific, regs(true, tes), false)], 1),
        (tables(true, false, None), vec![(No, regs(true, 0), false)], 1),
        (tables(false, false, Some("root table unreadable")), vec![], 0),
    ] {
        let f = dma_facts(&t, &u, found);
        let mut out = Capture::default();
        render_dma(&f, &mut out);
        let text = out.text();
        assert!(text.contains("DMA isolation NOT ESTABLISHED"), "{text}");
        assert!(text.contains("table present") && text.contains("hardware answers") && text.contains("translation enabled") && text.contains("xhci translated"));
        assert!(text.contains(&format!("xhci translated {}", f.xhci_translated.name())), "the last fact survives the line limit: {text}");
        assert!(text.lines().all(|l| l.len() <= 118 && l.starts_with("usb: ")), "{text}");
    }
}

#[test]
fn the_acpi_dmar_and_unit_lines_are_short_and_say_what_was_not_read() {
    let (mut m, rsdp) = world(&[(b"FACP", sdt(b"FACP", &[0; 8])), (b"DMAR", dmar(&[drhd(0, 0xfed9_0000, &[scope(1, 0, &[(0x14, 0)])]), rmrr(&[scope(1, 0, &[(0x14, 0)])])]))]);
    let t = walk_acpi(&mut m, &rsdp);
    let d = parse_dmar(&mut m, t.dmar.unwrap(), XHCI);
    let mut out = Capture::default();
    render_acpi(&Ok(rsdp), Some(&t), &mut out);
    render_dmar(&d, &mut out);
    render_unit(0, "intel", d.units[0].base, d.units[0].flags, Some(d.units[0].cover), None, &mut out);
    render_unit(1, "intel", 0xfed9_1000, 0, Some(Cover::No), Some(&regs(false, 0)), &mut out);
    render_unit(2, "intel", 0xfed9_2000, 0, Some(Cover::Specific), Some(&regs(true, 1 << 31)), &mut out);
    render_acpi(&Err("no ACPI RSDP tag in the boot information"), None, &mut out);
    let text = out.text();
    for want in [
        "usb: acpi rsdp revision 2 rsdt 0x00000000 xsdt 0x7ffe0000 root xsdt tables 2",
        "usb:   signatures FACP DMAR",
        "usb: dmar host-width 39 flags 0x01 units 1 rmrr 1 (an RMRR names the xhci: firmware DMA)",
        "usb:   intel unit 0 base 0xfed90000 flags 0x00 covers xhci: yes (named)\r\nusb:     registers NOT read",
        "usb:   intel unit 1 base 0xfed91000 flags 0x00 covers xhci: no\r\nusb:     registers do not answer",
        "covers xhci: yes (named)\r\nusb:     registers ver 0x10 status 0x80000000 translation ENABLED",
        "usb: acpi: no ACPI RSDP tag in the boot information (DMAR/IVRS not looked for)",
    ] {
        assert!(text.contains(want), "missing {want:?} in\n{text}");
    }
    assert!(text.lines().all(|l| l.len() <= 118), "{text}");
}

#[test]
fn the_longest_renderings_fit_the_console_line_and_lose_nothing() {
    let mut t = Tables { count: SIG_LIMIT as u8, more_than_listed: true, err: Some("a listed table is unreadable"), root_is_xsdt: true, ..Default::default() };
    for (i, s) in t.sigs.iter_mut().enumerate() {
        *s = [b'A' + i as u8, b'B', b'C', b'D'];
    }
    let mut out = Capture::default();
    render_acpi(&Ok(Rsdp { revision: 2, rsdt: u32::MAX, xsdt: u64::MAX }), Some(&t), &mut out);
    let d = Dmar { host_width: 48, flags: 255, n_units: 4, units_total: 255, rmrr: 255, rmrr_names_xhci: true, err: Some("DMAR structure length invalid"), ..Default::default() };
    render_dmar(&d, &mut out);
    let r = UnitRegs { responds: true, version: u32::MAX, cap: u64::MAX, ecap: u64::MAX, status: u32::MAX };
    render_unit(3, "intel", u64::MAX, 255, Some(Cover::PciAll), Some(&r), &mut out);
    render_unit(3, "amd", u64::MAX, 255, Some(Cover::Unknown), None, &mut out);
    let text = out.text();
    assert!(text.lines().all(|l| l.len() <= 118), "{text}");
    assert!(text.contains("XBCD"), "all {SIG_LIMIT} signatures are printed, not cut: {text}");
    assert!(text.contains("DMAR not fully read: DMAR structure length invalid") && text.contains("acpi tables not fully read"), "{text}");
    assert!(text.contains("tables 24+"), "{text}");
}

// ---- hostile ACPI memory -------------------------------------------------------------------------------------------------------

#[test]
fn random_acpi_memory_never_panics_and_every_read_stays_within_the_platform_bound() {
    let mut x = 0x9e37_79b9_7f4a_7c15u64;
    let mut next = || {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x
    };
    let mut parsed = 0;
    for round in 0..3000 {
        // start from a valid world and corrupt a few bytes, or fill with noise
        let (mut m, rsdp) = world(&[(b"DMAR", dmar(&[drhd(1, 0xfed9_0000, &[scope(1, 0, &[(0x14, 0)])]), rmrr(&[])])), (b"IVRS", ivrs(&[ivhd(0x10, 0xfeb8_0000)]))]);
        for _ in 0..(round % 6) {
            let keys: Vec<u64> = m.regions.keys().copied().collect();
            let k = keys[(next() % keys.len() as u64) as usize];
            let r = m.regions.get_mut(&k).unwrap();
            let i = (next() % r.len() as u64) as usize;
            r[i] = next() as u8;
        }
        if round % 7 == 0 {
            for r in m.regions.values_mut() {
                for b in r.iter_mut() {
                    *b = next() as u8;
                }
            }
        }
        let t = walk_acpi(&mut m, &rsdp);
        if let Some(pa) = t.dmar {
            let d = parse_dmar(&mut m, pa, XHCI);
            parsed += usize::from(d.err.is_none());
            assert!(usize::from(d.n_units) <= UNIT_LIMIT);
        }
        if let Some(pa) = t.ivrs {
            assert!(usize::from(parse_ivrs(&mut m, pa).n_units) <= UNIT_LIMIT);
        }
        assert!(m.reads < 2000, "{} reads", m.reads);
    }
    assert!(parsed > 100, "the generator rarely produced a parsable DMAR ({parsed})");
}

#[test]
fn the_firmware_source_has_no_unsafe_code_and_no_write_method() {
    let src = include_str!("../../kernel/rust/usb/xhci/firmware.rs");
    assert!(!src.contains("unsafe") && !src.contains("fn write"), "firmware.rs is safe Rust with read-only traits");
    assert!(src.contains("pub trait Phys {\n    fn read(&mut self, pa: u64, out: &mut [u8]) -> bool;"));
    let c = include_str!("../../arch/x86_64/xhci_survey.c");
    for bad in ["pci_write", "outb", "outw", "outl", "wrmsr", "write32", "write16", "write8", "memcpy", "memset"] {
        assert!(!c.contains(bad), "xhci_survey.c mentions {bad}");
    }
    assert!(c.contains("devmap_firmware_range_ok(pa, len)"), "the C read refuses anything the memory map does not call firmware memory");
    let d = include_str!("../../arch/x86_64/devmap.c");
    assert!(d.contains("\"USB input survey firmware table (read only)\", token, va, 1);"), "the firmware lease is registered read-only");
    assert!(d.contains("r->pa < (1ULL << 30) || !devmap_firmware_range_ok(r->pa, r->length)"), "and refused below the direct map or outside firmware memory");
}
