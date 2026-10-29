//! USB.2-C3a.2 pure Rust tests for the EFI firmware memory exclusion.
//! Fixtures ovmf-tag6.bin/ovmf-tag17.bin/seabios-tag6.bin are the raw
//! Multiboot2 tags of real QEMU boots (OVMF 4M, SeaBIOS) captured through the
//! gdbstub; the frame model below is independent of the implementation.
use vos_boot::efi_memory::{self, Descriptor, EfiMap, Error, Evidence, MemoryMap, Plan, Summary};

const LO: u64 = 1 << 20;
const HI: u64 = 1 << 30;
const PAGE: u64 = 4096;
const OVMF6: &[u8] = include_bytes!("fixtures/ovmf-tag6.bin");
const OVMF17: &[u8] = include_bytes!("fixtures/ovmf-tag17.bin");
const SEABIOS6: &[u8] = include_bytes!("fixtures/seabios-tag6.bin");
const WB: u64 = 1 << 3;

fn tag17(ds: u32, version: u32, descs: &[(u32, u64, u64, u64)]) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(&17u32.to_le_bytes());
    v.extend_from_slice(&((16 + descs.len() * ds as usize) as u32).to_le_bytes());
    v.extend_from_slice(&ds.to_le_bytes());
    v.extend_from_slice(&version.to_le_bytes());
    for &(kind, base, pages, attr) in descs {
        let mut d = vec![0u8; ds as usize];
        d[0..4].copy_from_slice(&kind.to_le_bytes());
        d[8..16].copy_from_slice(&base.to_le_bytes());
        d[16..24].copy_from_slice(&base.to_le_bytes());
        d[24..32].copy_from_slice(&pages.to_le_bytes());
        d[32..40].copy_from_slice(&attr.to_le_bytes());
        v.extend_from_slice(&d);
    }
    v
}

fn tag6(entries: &[(u64, u64, u32)]) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(&6u32.to_le_bytes());
    v.extend_from_slice(&((16 + 24 * entries.len()) as u32).to_le_bytes());
    v.extend_from_slice(&24u32.to_le_bytes());
    v.extend_from_slice(&0u32.to_le_bytes());
    for &(base, len, kind) in entries {
        v.extend_from_slice(&base.to_le_bytes());
        v.extend_from_slice(&len.to_le_bytes());
        v.extend_from_slice(&kind.to_le_bytes());
        v.extend_from_slice(&0u32.to_le_bytes());
    }
    v
}

fn run(t6: &[u8], t17: &[u8], capacity: usize) -> Result<(Vec<(u64, u64)>, Summary), Error> {
    let efi = EfiMap::parse(t17)?;
    let ram = MemoryMap::parse(t6)?;
    let mut out = Vec::new();
    let s = efi_memory::exclusions(&efi, &ram, LO, HI, capacity, &mut |a, b| {
        out.push((a, b));
        true
    })?;
    Ok((out, s))
}

fn le(b: &[u8], at: usize, n: usize) -> u64 {
    b[at..at + n].iter().rev().fold(0, |v, &x| (v << 8) | x as u64)
}

/// Independent frame model: tag-6 RAM frames (inward), allowed/other EFI coverage.
struct Model {
    ram: Vec<bool>,
    ok: Vec<bool>,
    bad: Vec<bool>,
}

fn model(t6: &[u8], t17: &[u8]) -> Model {
    let n = (HI / PAGE) as usize;
    let mut m = Model { ram: vec![false; n], ok: vec![false; n], bad: vec![false; n] };
    let es = le(t6, 8, 4) as usize;
    let mut at = 16;
    while at + es <= t6.len() {
        let (base, len, kind) = (le(t6, at, 8), le(t6, at + 8, 8), le(t6, at + 16, 4));
        if kind == 1 && len > 0 && base.checked_add(len).is_some() {
            let s = base.max(LO).div_ceil(PAGE);
            let e = (base + len).min(HI) / PAGE;
            for f in s..e.max(s) {
                m.ram[f as usize] = true;
            }
        }
        at += es;
    }
    let ds = le(t17, 8, 4) as usize;
    let mut at = 16;
    while at + ds <= t17.len() {
        let (kind, base, pages, attr) = (le(t17, at, 4), le(t17, at + 8, 8), le(t17, at + 24, 8), le(t17, at + 32, 8));
        let allowed = matches!(kind, 1 | 2 | 7) && attr & WB != 0 && attr & ((1 << 63) | (1 << 18)) == 0;
        for f in (base / PAGE)..(base / PAGE + pages).min(HI / PAGE) {
            if allowed { m.ok[f as usize] = true } else { m.bad[f as usize] = true }
        }
        at += ds;
    }
    m
}

impl Model {
    fn must_exclude(&self, f: usize) -> bool {
        f as u64 >= LO / PAGE && self.ram[f] && (self.bad[f] || !self.ok[f])
    }
}

/// Model PMM after applying exclusions through a clear-only, outward-rounded path.
fn allocatable(m: &Model, ranges: &[(u64, u64)]) -> Vec<bool> {
    let mut a: Vec<bool> = m.ram.iter().enumerate().map(|(f, &r)| r && f as u64 >= LO / PAGE).collect();
    for &(s, e) in ranges {
        for f in (s / PAGE)..e.div_ceil(PAGE).min(HI / PAGE) {
            a[f as usize] = false;
        }
    }
    a
}

#[test]
fn ovmf_fixture_excludes_exactly_boot_services_and_firmware_dma_pages() {
    let (ranges, s) = run(OVMF6, OVMF17, 64).unwrap();
    let m = model(OVMF6, OVMF17);
    let a = allocatable(&m, &ranges);
    let want: usize = (0..a.len()).filter(|&f| m.must_exclude(f)).count();
    assert_eq!(s.frames as usize, want);
    assert_eq!(want, 9765, "OVMF BootServices frames in [1 MiB, 1 GiB)");
    assert!(!s.coalesced && s.ranges as usize == ranges.len());
    for f in 0..a.len() {
        assert_eq!(a[f], m.ram[f] && f as u64 >= LO / PAGE && !m.must_exclude(f), "frame {f:#x}");
    }
    // xHCI DCBAA/command/event/ERST and armed AHCI command list/FIS pages of that boot.
    for p in [0x674a000u64, 0x674b000, 0x674c000, 0x674d000, 0x6744000, 0x6745000] {
        assert!(!a[(p / PAGE) as usize], "{p:#x} must not be allocatable");
    }
    assert!(ranges.windows(2).all(|w| w[0].1 < w[1].0), "ascending, disjoint, merged");
    let efi = EfiMap::parse(OVMF17).unwrap();
    assert_eq!(efi.len(), 121);
    let bs = (0..efi.len()).filter_map(|i| efi.descriptor(i)).filter(|d| matches!(d.kind, 3 | 4)).count();
    assert_eq!(bs, 90);
}

#[test]
fn bios_and_policy_decisions_fail_closed() {
    assert!(MemoryMap::parse(SEABIOS6).is_ok());
    let e = |efi_firmware, efi_map_tags, boot_services_active| Evidence { efi_firmware, efi_map_tags, boot_services_active };
    assert_eq!(efi_memory::plan(&e(false, 0, false)), Ok(Plan::NotApplicable));
    assert_eq!(efi_memory::plan(&e(true, 1, false)), Ok(Plan::Apply));
    assert_eq!(efi_memory::plan(&e(false, 1, false)), Ok(Plan::Apply), "an EFI map is honoured even without a systab tag");
    assert_eq!(efi_memory::plan(&e(true, 0, false)), Err(Error::Missing));
    assert_eq!(efi_memory::plan(&e(true, 2, false)), Err(Error::Duplicate));
    assert_eq!(efi_memory::plan(&e(true, 1, true)), Err(Error::BootServicesActive));
    assert_eq!(efi_memory::plan(&e(false, 0, true)), Err(Error::BootServicesActive));
}

#[test]
fn malformed_efi_maps_are_rejected() {
    let ok = tag17(48, 1, &[(7, 0x100000, 16, WB)]);
    assert!(EfiMap::parse(&ok).is_ok());
    assert_eq!(EfiMap::parse(&ok[..ok.len() - 1]).err(), Some(Error::Truncated), "truncated bytes");
    assert_eq!(EfiMap::parse(&ok[..12]).err(), Some(Error::Truncated), "shorter than the header");
    assert_eq!(EfiMap::parse(&[]).err(), Some(Error::Truncated));
    let mut wrong = ok.clone();
    wrong[0] = 6;
    assert_eq!(EfiMap::parse(&wrong).err(), Some(Error::Truncated), "wrong tag type");
    let mut big = ok.clone();
    big[4..8].copy_from_slice(&1000u32.to_le_bytes());
    assert_eq!(EfiMap::parse(&big).err(), Some(Error::Truncated), "size beyond the bytes");
    for ds in [0u32, 8, 32, 44, 1032] {
        let mut t = tag17(48, 1, &[(7, 0x100000, 16, WB)]);
        t[8..12].copy_from_slice(&ds.to_le_bytes());
        assert_eq!(EfiMap::parse(&t).err(), Some(Error::DescriptorSize), "descriptor size {ds}");
    }
    for v in [0u32, 2] {
        assert_eq!(EfiMap::parse(&tag17(48, v, &[(7, 0x100000, 16, WB)])).err(), Some(Error::Version));
    }
    assert_eq!(EfiMap::parse(&tag17(48, 1, &[])).err(), Some(Error::Count), "no descriptors");
    let mut partial = tag17(48, 1, &[(7, 0x100000, 16, WB)]);
    partial.extend_from_slice(&[0; 8]);
    let n = partial.len() as u32;
    partial[4..8].copy_from_slice(&n.to_le_bytes());
    assert_eq!(EfiMap::parse(&partial).err(), Some(Error::Count), "partial descriptor");
    let many: Vec<_> = (0..513u64).map(|i| (7, 0x100000 + i * PAGE, 1, WB)).collect();
    assert_eq!(EfiMap::parse(&tag17(48, 1, &many)).err(), Some(Error::Count), "too many descriptors");
    assert_eq!(EfiMap::parse(&tag17(48, 1, &[(7, 0x100800, 1, WB)])).err(), Some(Error::Unaligned));
    assert_eq!(EfiMap::parse(&tag17(48, 1, &[(7, 0xffff_ffff_ffff_f000, 2, WB)])).err(), Some(Error::Overflow),
               "base + length wraps");
    assert_eq!(EfiMap::parse(&tag17(48, 1, &[(7, 0, 1 << 53, WB)])).err(), Some(Error::Overflow), "pages * 4 KiB wraps");
    // Larger descriptors (newer firmware) parse; zero-page descriptors cover nothing.
    let wide_tag = tag17(64, 1, &[(7, 0x100000, 16, WB), (4, 0x200000, 0, WB)]);
    let wide = EfiMap::parse(&wide_tag).unwrap();
    assert_eq!(wide.descriptor(0), Some(Descriptor { kind: 7, start: 0x100000, end: 0x110000, attribute: WB }));
    assert_eq!(wide.descriptor(1), None);
    // The tag-6 map is validated too.
    let t6 = tag6(&[(0x100000, 0x100000, 1)]);
    assert!(MemoryMap::parse(&t6).is_ok());
    assert_eq!(MemoryMap::parse(&t6[..t6.len() - 1]).err(), Some(Error::MemoryMap));
    let mut v1 = t6.clone();
    v1[12] = 1;
    assert_eq!(MemoryMap::parse(&v1).err(), Some(Error::MemoryMap), "tag-6 entry version");
    assert_eq!(MemoryMap::parse(&tag6(&[])).err(), Some(Error::MemoryMap), "empty tag-6 map");
}

#[test]
fn overlaps_contradictions_and_attributes_resolve_toward_exclusion() {
    let t6 = tag6(&[(0x100000, 0x400000, 1)]);
    let mb = |base: u64| 0x100000 + base * 0x100000;
    let cases: &[(&str, Vec<(u32, u64, u64, u64)>, &[u64])] = &[
        ("conventional overlapped by boot services data", vec![(7, mb(0), 1024, WB), (4, mb(1), 256, WB)], &[1]),
        ("allowed overlapping allowed", vec![(7, mb(0), 1024, WB), (2, mb(1), 256, WB)], &[]),
        ("conventional without WB", vec![(7, mb(0), 768, WB), (7, mb(3), 256, 1)], &[3]),
        ("conventional with RUNTIME", vec![(7, mb(0), 768, WB), (7, mb(3), 256, WB | 1 << 63)], &[3]),
        ("specific-purpose memory", vec![(7, mb(0), 768, WB), (7, mb(3), 256, WB | 1 << 18)], &[3]),
        ("unaccepted memory (type 15)", vec![(7, mb(0), 768, WB), (15, mb(3), 256, WB)], &[3]),
        ("OEM type", vec![(7, mb(0), 768, WB), (0x7000_0000, mb(3), 256, WB)], &[3]),
        ("reserved inside tag-6 RAM", vec![(7, mb(0), 768, WB), (0, mb(3), 256, WB)], &[3]),
        ("boot services code", vec![(7, mb(0), 768, WB), (3, mb(3), 256, WB)], &[3]),
        ("tag-6 RAM with no EFI descriptor", vec![(7, mb(0), 512, WB), (1, mb(3), 256, WB)], &[2]),
    ];
    for (what, descs, excluded_mib) in cases {
        let t17 = tag17(48, 1, descs);
        let (ranges, s) = run(&t6, &t17, 16).unwrap();
        let want: Vec<(u64, u64)> = excluded_mib.iter().map(|&m| (mb(m), mb(m + 1))).collect();
        assert_eq!(ranges, want, "{what}");
        assert_eq!(s.frames, 256 * excluded_mib.len() as u64, "{what}");
    }
}

#[test]
fn exclusions_never_add_allocatable_memory() {
    // EFI claims conventional RAM over tag-6 reserved memory and beyond RAM.
    let t6 = tag6(&[(0x100000, 0x100000, 1), (0x200000, 0x100000, 2)]);
    let t17 = tag17(48, 1, &[(7, 0x100000, 4096, WB)]);
    let (ranges, s) = run(&t6, &t17, 16).unwrap();
    assert!(ranges.is_empty() && s.frames == 0, "nothing removed, and nothing can be added");
    let m = model(&t6, &t17);
    let a = allocatable(&m, &ranges);
    assert_eq!(a.iter().filter(|&&x| x).count(), 256, "only the tag-6 RAM megabyte");
    // Randomized maps: the result is always a subset of tag-6 RAM, never contains
    // a frame the model must exclude and, when not coalesced, keeps every other one.
    let mut x = 0x9e3779b97f4a7c15u64;
    let mut rnd = |n: u64| {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x % n
    };
    let kinds = [0u32, 1, 2, 3, 4, 5, 6, 7, 7, 7, 9, 10, 11, 14, 15];
    for round in 0..300 {
        let entries: Vec<(u64, u64, u32)> = (0..1 + rnd(6))
            .map(|_| (rnd(300) * 0x10000 + rnd(2) * 0x800, (1 + rnd(200)) * 0x10000 + rnd(2) * 0x400,
                      if rnd(4) == 0 { 2 } else { 1 }))
            .collect();
        let descs: Vec<(u32, u64, u64, u64)> = (0..1 + rnd(20))
            .map(|_| (kinds[rnd(kinds.len() as u64) as usize], rnd(300) * 0x10000, 1 + rnd(4000),
                      match rnd(6) { 0 => 0, 1 => WB | 1 << 63, _ => WB }))
            .collect();
        let (t6, t17) = (tag6(&entries), tag17(48, 1, &descs));
        let capacity = if round % 5 == 0 { 1 + rnd(3) as usize } else { 64 };
        let (ranges, s) = run(&t6, &t17, capacity).unwrap();
        assert!(ranges.len() <= capacity && ranges.windows(2).all(|w| w[0].1 <= w[1].0), "round {round}");
        let m = model(&t6, &t17);
        let a = allocatable(&m, &ranges);
        for f in 0..a.len() {
            assert!(!a[f] || m.ram[f], "round {round}: frame {f:#x} added");
            assert!(!(a[f] && m.must_exclude(f)), "round {round}: frame {f:#x} not excluded");
            if !s.coalesced && m.ram[f] && f as u64 >= LO / PAGE && !m.must_exclude(f) {
                // Partial frames at unaligned tag-6 edges may be removed (outward); whole ones stay.
                let edge = ranges.iter().any(|&(s0, e0)| s0 / PAGE == f as u64 || e0.div_ceil(PAGE) - 1 == f as u64);
                assert!(a[f] || edge, "round {round}: frame {f:#x} lost");
            }
        }
    }
}

#[test]
fn partially_covered_frames_are_excluded_outward() {
    // Tag-6 RAM ends mid-page inside a boot-services descriptor.
    let t6 = tag6(&[(0x100000, 0x100800, 1)]);
    let t17 = tag17(48, 1, &[(7, 0x100000, 256, WB), (4, 0x200000, 16, WB)]);
    let (ranges, s) = run(&t6, &t17, 16).unwrap();
    assert_eq!(ranges, vec![(0x200000, 0x200800)]);
    assert_eq!(s.frames, 1, "the partial frame counts and is removed whole");
    let a = allocatable(&model(&t6, &t17), &ranges);
    assert!(!a[0x200] && a[0x1ff]);
}

#[test]
fn capacity_coalesces_conservatively_and_fails_closed() {
    let t6 = tag6(&[(0x100000, 0x1000000, 1)]);
    let descs = [(7, 0x100000, 0x1000, WB), (4, 0x200000, 16, WB), (4, 0x400000, 16, WB), (4, 0x600000, 16, WB)];
    let t17 = tag17(48, 1, &descs);
    let (r3, s3) = run(&t6, &t17, 3).unwrap();
    assert_eq!(r3, vec![(0x200000, 0x210000), (0x400000, 0x410000), (0x600000, 0x610000)]);
    assert!(!s3.coalesced && s3.frames == 48);
    let (r2, s2) = run(&t6, &t17, 2).unwrap();
    assert_eq!(r2, vec![(0x200000, 0x210000), (0x400000, 0x610000)], "the last slot widens over the gap");
    assert!(s2.coalesced && s2.ranges == 2);
    let (r1, s1) = run(&t6, &t17, 1).unwrap();
    assert_eq!(r1, vec![(0x200000, 0x610000)]);
    assert!(s1.coalesced && s1.frames == (0x610000 - 0x200000) / PAGE);
    assert_eq!(run(&t6, &t17, 0).err(), Some(Error::Capacity));
    let clean = tag17(48, 1, &[(7, 0x100000, 0x1000, WB)]);
    assert_eq!(run(&t6, &clean, 0).map(|(r, _)| r.len()), Ok(0), "nothing to exclude needs no capacity");
}

#[test]
fn window_and_sink_failures_are_errors() {
    let t6 = tag6(&[(0x100000, 0x100000, 1)]);
    let t17 = tag17(48, 1, &[(4, 0x100000, 256, WB)]);
    let (efi, ram) = (EfiMap::parse(&t17).unwrap(), MemoryMap::parse(&t6).unwrap());
    let mut keep = |_: u64, _: u64| true;
    assert_eq!(efi_memory::exclusions(&efi, &ram, HI, LO, 8, &mut keep).err(), Some(Error::Window));
    assert_eq!(efi_memory::exclusions(&efi, &ram, LO + 1, HI, 8, &mut keep).err(), Some(Error::Window));
    assert_eq!(efi_memory::exclusions(&efi, &ram, LO, HI - 1, 8, &mut keep).err(), Some(Error::Window));
    let mut refuse = |_: u64, _: u64| false;
    assert_eq!(efi_memory::exclusions(&efi, &ram, LO, HI, 8, &mut refuse).err(), Some(Error::Sink));
}

#[test]
fn allocator_model_never_hands_out_excluded_frames() {
    // Negative control: first-fit over the model PMM, with and without exclusion.
    let m = model(OVMF6, OVMF17);
    let firmware = [0x674a000u64, 0x674b000, 0x674c000, 0x674d000, 0x6744000, 0x6745000];
    let without = allocatable(&m, &[]);
    let handed: Vec<u64> = (0..without.len()).filter(|&f| without[f]).map(|f| f as u64 * PAGE).collect();
    assert!(firmware.iter().all(|p| handed.contains(p)), "without exclusion first-fit reaches firmware DMA pages");
    let (ranges, _) = run(OVMF6, OVMF17, 64).unwrap();
    let with = allocatable(&m, &ranges);
    let mut taken = vec![false; with.len()];
    for f in 0..with.len() {
        if with[f] {
            taken[f] = true;            // allocate until exhausted
        }
    }
    assert!(firmware.iter().all(|&p| !taken[(p / PAGE) as usize]), "exhaustion never reaches excluded frames");
    assert!((0..with.len()).all(|f| !(taken[f] && m.must_exclude(f))));
    // A request for a specific excluded frame finds it unavailable.
    assert!(firmware.iter().all(|&p| !with[(p / PAGE) as usize]));
}

// ---- USB.2-C3a.3: excluded firmware RAM that may be inspected in place ----

fn frame(t6: &[u8], t17: &[u8], f: u64) -> bool {
    efi_memory::firmware_frame(&EfiMap::parse(t17).unwrap(), &MemoryMap::parse(t6).unwrap(), f, LO, HI)
}

#[test]
fn firmware_frames_are_exactly_the_whole_excluded_ram_frames() {
    // OVMF: the xHCI and AHCI firmware pages are excluded RAM; every frame agrees
    // with the independent model restricted to whole tag-6 RAM frames.
    for p in [0x674a000u64, 0x674b000, 0x674c000, 0x674d000, 0x6744000, 0x6745000] {
        assert!(frame(OVMF6, OVMF17, p), "{p:#x}");
    }
    let m = model(OVMF6, OVMF17);
    let (efi, ram) = (EfiMap::parse(OVMF17).unwrap(), MemoryMap::parse(OVMF6).unwrap());
    let mut yes = 0;
    for f in (0..HI / PAGE).step_by(7) {        // a deterministic sample across the window
        let got = efi_memory::firmware_frame(&efi, &ram, f * PAGE, LO, HI);
        assert_eq!(got, m.must_exclude(f as usize), "frame {:#x}", f * PAGE);
        yes += got as u32;
    }
    assert!(yes > 1000);
    // Outside the window, unaligned, or at the 64-bit edge: never.
    assert!(!frame(OVMF6, OVMF17, 0x9000) && !frame(OVMF6, OVMF17, HI) && !frame(OVMF6, OVMF17, 0x674a008));
    assert!(!frame(OVMF6, OVMF17, u64::MAX & !(PAGE - 1)));
    // Allowed (conventional/loader) RAM is never inspectable firmware memory.
    let conventional = (LO / PAGE..HI / PAGE).find(|&f| m.ram[f as usize] && !m.must_exclude(f as usize)).unwrap();
    assert!(!frame(OVMF6, OVMF17, conventional * PAGE));
}

#[test]
fn firmware_frames_require_whole_ram_and_no_unavailable_overlap() {
    // Tag-6 RAM ends mid-page inside boot-services memory: that frame is removed
    // (outward) but is not whole RAM, so it is not inspectable.
    let t6 = tag6(&[(0x100000, 0x100800, 1)]);
    let t17 = tag17(48, 1, &[(7, 0x100000, 256, WB), (4, 0x200000, 16, WB)]);
    assert!(!frame(&t6, &t17, 0x200000));
    // A non-available entry touching a boot-services frame makes it uninspectable.
    let t6 = tag6(&[(0x100000, 0x300000, 1), (0x300800, 0x100, 2)]);
    let t17 = tag17(48, 1, &[(7, 0x100000, 512, WB), (4, 0x300000, 256, WB)]);
    assert!(!frame(&t6, &t17, 0x300000) && frame(&t6, &t17, 0x301000));
    // A conventional gap between two excluded ranges (what a coalesced
    // reservation would also cover) is not firmware memory.
    let t6 = tag6(&[(0x100000, 0x1000000, 1)]);
    let t17 = tag17(48, 1, &[(7, 0x100000, 0x1000, WB), (4, 0x200000, 16, WB), (4, 0x400000, 16, WB)]);
    assert!(frame(&t6, &t17, 0x200000) && frame(&t6, &t17, 0x400000) && !frame(&t6, &t17, 0x300000));
    // Uncovered tag-6 RAM is excluded and inspectable; RAM the EFI map calls
    // conventional but tag 6 does not list is neither.
    let t6 = tag6(&[(0x100000, 0x300000, 1)]);
    let t17 = tag17(48, 1, &[(7, 0x100000, 256, WB), (7, 0x300000, 0x1000, WB)]);
    assert!(frame(&t6, &t17, 0x200000) && !frame(&t6, &t17, 0x500000));
    // The window must itself be page-aligned.
    let t6 = tag6(&[(0x100000, 0x300000, 1)]);
    let t17 = tag17(48, 1, &[(4, 0x100000, 768, WB)]);
    let (efi, ram) = (EfiMap::parse(&t17).unwrap(), MemoryMap::parse(&t6).unwrap());
    assert!(efi_memory::firmware_frame(&efi, &ram, 0x200000, LO, HI));
    assert!(!efi_memory::firmware_frame(&efi, &ram, 0x200000, LO + 1, HI));
}
