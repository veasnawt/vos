//! USB.2-C1 pure qualification tests: the canonical crate, no platform access.
#![forbid(unsafe_code)]
use vos_xhci::xhci::capabilities::{Bytes, Capabilities, Words, inspect};
use vos_xhci::xhci::qualify::{self, Config, Reason, check_profile, inspect_pci, observe, select};
use vos_xhci::xhci::{BAR_BYTES, Error};
pub mod xhci_fake;
use xhci_fake::{capabilities, put};

/// Configuration space of one selected function, as the C reader exposes it.
#[derive(Clone)]
struct Cfg {
    w: [u32; 64],
    reads: u32,
    fail: bool,
}
impl Config for Cfg {
    fn read32(&mut self, offset: u8) -> Result<u32, Reason> {
        // Platform contract: aligned offsets below 256 only.
        assert_eq!(offset % 4, 0, "qualifier issued an unaligned configuration read");
        self.reads += 1;
        if self.fail {
            return Err(Reason::Interface);
        }
        Ok(self.w[usize::from(offset / 4)])
    }
}
/// The observed QEMU q35 qemu-xhci function: BAR0 64-bit at 0xfebf0000,
/// MSI-X at 0x90 (16 vectors, table 0x3000, PBA 0x3800), PCIe at 0xa0.
fn qemu() -> Cfg {
    let mut w = [0u32; 64];
    w[0] = 0x000d_1b36;
    w[1] = 0x0010_0107; // status: capability list; command: IO|MEM|BME|SERR
    w[2] = 0x0c03_3001;
    w[3] = 0;
    w[4] = 0xfebf_0004;
    w[5] = 0;
    w[0x34 / 4] = 0x90;
    w[0x90 / 4] = 0x000f_a011;
    w[0x94 / 4] = 0x3000;
    w[0x98 / 4] = 0x3800;
    w[0xa0 / 4] = 0x0002_0010;
    Cfg { w, reads: 0, fail: false }
}
fn pci(c: &mut Cfg) -> Result<qualify::Pci, Reason> {
    inspect_pci(c, 40)
}
fn cap(c: &mut Cfg, at: u32, id: u32, next: u32, body: &[u32]) {
    c.w[at as usize / 4] = id | (next << 8) | (c.w[at as usize / 4] & 0xffff_0000);
    for (i, v) in body.iter().enumerate() {
        c.w[at as usize / 4 + 1 + i] = *v;
    }
}

#[test]
fn qemu_profile_qualifies() {
    let mut c = qemu();
    let p = pci(&mut c).unwrap();
    assert_eq!((p.vendor, p.device, p.base, p.bar_bits), (0x1b36, 0xd, 0xfebf_0000, 64));
    assert_eq!((p.pm, p.power_state, p.msi, p.msix, p.capabilities), (0, 0, 0, 0x90, 2));
    assert_eq!((p.msix_table, p.msix_pba, p.state.msix_control), (0x3000, 0x3800, 0xf));
    assert_eq!(p.state.command, 0x107, "firmware-set BME is observed, not changed");
    assert_eq!(observe(&mut c, &p).unwrap(), p.state);
    assert!(c.reads <= 16, "bounded configuration reads: {}", c.reads);
}

#[test]
fn candidate_selection() {
    assert_eq!(select(0, true), Err(Reason::NotFound));
    assert_eq!(select(2, true), Err(Reason::Ambiguous));
    assert_eq!(select(1, false), Err(Reason::IncompleteScan));
    assert_eq!(select(0, false), Err(Reason::IncompleteScan));
    assert_eq!(select(1, true), Ok(()));
}

#[test]
fn identity_and_header() {
    for (word, value, reason) in [
        (0, 0xffff_ffff, Reason::Absent),
        (2, 0x0c03_2001, Reason::Class), // EHCI programming interface
        (2, 0x0c02_3001, Reason::Class),
        (3, 0x0001_0000, Reason::Header), // type-1 bridge header
        (0, 0x000d_0000, Reason::Header), // vendor zero
        (0, 0x0194_1033, Reason::Profile), // QEMU NEC xHCI: unknown, fails closed
        (0, 0x000e_1b36, Reason::Profile),
    ] {
        let mut c = qemu();
        c.w[word] = value;
        assert_eq!(pci(&mut c), Err(reason), "word {word} = {value:#x}");
    }
    let mut c = qemu();
    c.w[3] = 0x0080_0000; // multi-function bit alone is not a header-type change
    assert!(pci(&mut c).is_ok());
    let mut c = qemu();
    c.fail = true;
    assert_eq!(pci(&mut c), Err(Reason::Interface));
}

#[test]
fn bar_form_extent_and_width() {
    for (low, high, bits, reason) in [
        (0xfebf_0001, 0, 40, Reason::IoBar),
        (0x000f_0002, 0, 40, Reason::LowBar),
        (0xfebf_0006, 0, 40, Reason::ReservedBar),
        (0x0000_0004, 0, 40, Reason::Unassigned),
        (0xfebf_1004, 0, 40, Reason::BarAlignment), // page- but not 16 KiB-aligned
        (0xfebf_2004, 0, 40, Reason::BarAlignment),
        (0xffff_c004, 0xffff_ffff, 52, Reason::BarOverflow),
        (0x0000_0004, 0x100, 40, Reason::PhysicalWidth), // exactly 2^40
        (0x0000_4004, 0x100, 40, Reason::PhysicalWidth),
    ] {
        let mut c = qemu();
        c.w[4] = low;
        c.w[5] = high;
        assert_eq!(inspect_pci(&mut c, bits), Err(reason), "BAR {high:#x}:{low:#x}");
    }
    let mut c = qemu();
    c.w[4] = 0xffff_c000; // 32-bit BAR whose extent ends exactly at 4 GiB
    assert_eq!(pci(&mut c).unwrap().base, 0xffff_c000);
    let mut c = qemu();
    c.w[4] = 0xffff_c004; // extent ends exactly at 2^40: accepted
    c.w[5] = 0xff;
    assert_eq!(inspect_pci(&mut c, 40).unwrap().base, 0xff_ffff_c000);
    let mut c = qemu();
    c.w[4] = 0xffff_c004;
    c.w[5] = 0x0f;
    assert_eq!(inspect_pci(&mut c, 40).unwrap().base, 0xf_ffff_c000);
    for bits in [0, 31, 53, 64] {
        assert_eq!(inspect_pci(&mut qemu(), bits), Err(Reason::Interface));
    }
}

#[test]
fn command_and_capability_presence() {
    let mut c = qemu();
    c.w[1] &= !2;
    assert_eq!(pci(&mut c), Err(Reason::MemoryDisabled));
    let mut c = qemu();
    c.w[1] &= !(1 << 20);
    assert_eq!(pci(&mut c), Err(Reason::NoCapabilities));
}

#[test]
fn capability_list_offsets_cycles_and_limit() {
    for pointer in [0x3c, 0x04, 0x91, 0x92, 0xfe, 0xff] {
        let mut c = qemu();
        c.w[0x34 / 4] = pointer;
        assert_eq!(pci(&mut c), Err(Reason::CapabilityOffset), "pointer {pointer:#x}");
    }
    let mut c = qemu();
    cap(&mut c, 0xa0, 0x10, 0x90, &[]); // 0x90 -> 0xa0 -> 0x90
    assert_eq!(pci(&mut c), Err(Reason::CapabilityCycle));
    let mut c = qemu();
    cap(&mut c, 0x90, 0x11, 0x90, &[0x3000, 0x3800]); // self loop
    assert_eq!(pci(&mut c), Err(Reason::CapabilityCycle));
    // All 48 aligned slots, terminated: accepted. Each entry is vendor-specific.
    let chain = |terminal: u32| {
        let mut c = qemu();
        c.w[0x34 / 4] = 0x40;
        for at in (0x40..=0xfc).step_by(4) {
            let next = if at == 0xfc { terminal } else { at + 4 };
            c.w[at as usize / 4] = 0x09 | (next << 8);
        }
        c
    };
    let mut c = chain(0);
    assert_eq!(pci(&mut c).unwrap().capabilities, 48);
    // The 49th link exists: the bound stops before it can be followed.
    let mut c = chain(0x40);
    assert_eq!(pci(&mut c), Err(Reason::CapabilityLimit));
    let mut c = qemu();
    cap(&mut c, 0xa0, 0x10, 0xfc, &[]);
    c.w[0xfc / 4] = 0x0002_0001; // PM capability whose PMCSR would be at 0x100
    assert_eq!(pci(&mut c), Err(Reason::CapabilityOffset));
    let mut c = qemu();
    c.w[0x34 / 4] = 0xf8;
    c.w[0xf8 / 4] = 0x000f_0011; // MSI-X whose PBA word would be at 0x100
    c.w[0xfc / 4] = 0x3000;
    assert_eq!(pci(&mut c), Err(Reason::CapabilityOffset));
}

#[test]
fn duplicate_capabilities() {
    for (id, body) in [(0x01u32, vec![0u32]), (0x05, vec![]), (0x11, vec![0x3000, 0x3800])] {
        let mut c = qemu();
        cap(&mut c, 0xa0, 0x10, 0xb0, &[]);
        cap(&mut c, 0xb0, id, 0xc0, &body);
        cap(&mut c, 0xc0, id, 0, &body);
        assert_eq!(pci(&mut c), Err(Reason::DuplicateCapability), "id {id:#x}");
    }
}

#[test]
fn power_states() {
    for state in 0..4u32 {
        let mut c = qemu();
        cap(&mut c, 0xa0, 0x10, 0xb0, &[]);
        cap(&mut c, 0xb0, 0x01, 0, &[0x0008 | state]); // PMCSR NoSoftReset | state
        let result = pci(&mut c);
        if state == 0 {
            let p = result.unwrap();
            assert_eq!((p.pm, p.power_state, p.state.pmcsr), (0xb0, 0, 8));
        } else {
            assert_eq!(result, Err(Reason::PowerState), "D{state}");
        }
    }
}

#[test]
fn msix_layout_inside_profile_bar() {
    for (control, table, pba) in [
        (0x000f, 0x3001, 0x3800), // BIR 1
        (0x000f, 0x3000, 0x3804), // PBA BIR 4
        (0x000f, 0x3f10, 0x3800), // table end 0x4010 > 16 KiB
        (0x000f, 0x3000, 0x4000), // PBA at the extent end
        (0x07ff, 0x0000, 0x3800), // 2048 vectors = 32 KiB table
        (0x000f, 0xffff_fff8, 0x3800),
    ] {
        let mut c = qemu();
        c.w[0x90 / 4] = (control << 16) | 0xa011;
        c.w[0x94 / 4] = table;
        c.w[0x98 / 4] = pba;
        assert_eq!(pci(&mut c), Err(Reason::MsixLayout), "{control:#x} {table:#x} {pba:#x}");
    }
    let mut c = qemu();
    c.w[0x94 / 4] = 0x3f00; // 16 vectors end exactly at 16 KiB
    c.w[0x98 / 4] = 0x3ff8;
    assert!(pci(&mut c).is_ok());
}

#[test]
fn observation_detects_every_compared_field() {
    let mut c = qemu();
    cap(&mut c, 0xa0, 0x10, 0xb0, &[]);
    cap(&mut c, 0xb0, 0x01, 0xc0, &[0]);
    cap(&mut c, 0xc0, 0x05, 0, &[]);
    let p = pci(&mut c).unwrap();
    assert_eq!(observe(&mut c, &p).unwrap(), p.state);
    for (word, flip) in [(1, 4u32), (4, 0x10_0000), (5, 1), (0xb4 / 4, 3), (0xc0 / 4, 1 << 16), (0x90 / 4, 1 << 31)] {
        let mut changed = c.clone();
        changed.w[word] ^= flip;
        assert_ne!(observe(&mut changed, &p).unwrap(), p.state, "word {word}");
    }
    let mut status = c.clone();
    status.w[1] ^= 0x8000_0000; // RW1C status bits are not compared
    assert_eq!(observe(&mut status, &p).unwrap(), p.state);
    let mut failing = c.clone();
    failing.fail = true;
    assert_eq!(observe(&mut failing, &p), Err(Reason::Interface));
    assert_eq!(check_profile(&p), Ok(()));
}

// ---- copied xHCI capability region -------------------------------------

/// QEMU qemu-xhci as observed: two protocol capabilities, USB2 1-4 and USB3 5-8.
fn qemu_bar() -> Vec<u8> {
    let (mut b, _) = capabilities();
    put(&mut b, 0x20, 0x0200_0402); // USB 2.0, next 0x10 bytes
    put(&mut b, 0x28, 0x0000_0401);
    put(&mut b, 0x30, 0x0300_0002); // USB 3.0, last
    put(&mut b, 0x34, 0x2042_5355);
    put(&mut b, 0x38, 0x0000_0405);
    b.to_vec()
}
fn inspect_bytes(b: &[u8]) -> Result<vos_xhci::xhci::capabilities::Inspection, Error> {
    inspect(&mut Bytes(b))
}

#[test]
fn inspect_observed_profile_and_parse_equivalence() {
    let b = qemu_bar();
    let i = inspect_bytes(&b).unwrap();
    assert_eq!((i.extended, i.protocol_ports, i.pagesize, i.legacy), (2, 0xff, 1, None));
    assert_eq!((i.operational, i.doorbell, i.runtime, i.ports, i.slots), (0x40, 0x2000, 0x1000, 8, 64));
    let (fake, facts) = capabilities();
    let c = Capabilities::parse(&fake, facts).unwrap();
    let j = inspect_bytes(&fake).unwrap();
    assert_eq!((c.operational, c.doorbell, c.runtime, c.legacy), (j.operational, j.doorbell, j.runtime, None));
}

/// Legacy support at 0x20 (next 0x10 bytes), protocol at 0x30.
fn with_legacy(usblegsup: u32, usblegctlsts: u32) -> Vec<u8> {
    let mut b = qemu_bar();
    put(&mut b, 0x20, (usblegsup & !0xffff) | 0x0401);
    put(&mut b, 0x24, usblegctlsts);
    put(&mut b, 0x28, 0);
    put(&mut b, 0x38, 0x0000_0801);
    b
}

#[test]
fn legacy_semaphores_observed_only() {
    for (sup, bios, os) in [(0x0001_0000u32, true, false), (0x0100_0000, false, true), (0, false, false), (0x0101_0000, true, true)] {
        let b = with_legacy(sup, 0xe000_2001);
        let i = inspect_bytes(&b).unwrap();
        let l = i.legacy.expect("legacy support");
        assert_eq!((l.offset, l.usblegctlsts, l.bios_owned(), l.os_owned()), (0x20, 0xe000_2001, bios, os));
        assert_eq!(l.usblegsup, sup | 0x0401);
        let (_, facts) = capabilities();
        let mut full = [0u8; BAR_BYTES as usize];
        full.copy_from_slice(&b);
        assert_eq!(Capabilities::parse(&full, facts).unwrap().legacy, Some(0x20));
    }
}

#[test]
fn duplicate_and_malformed_extended_capabilities() {
    // Legacy 0x20 -> legacy 0x28 -> protocol 0x30: only the duplicate rule rejects.
    let mut b = with_legacy(0x0001_0000, 0);
    put(&mut b, 0x20, 0x0001_0201);
    put(&mut b, 0x28, 0x0000_0201); // second legacy capability, well formed
    put(&mut b, 0x2c, 0);
    assert_eq!(inspect_bytes(&b), Err(Error::InvalidCapability));
    put(&mut b, 0x28, 0x0000_02c0); // same chain with an unknown ID is accepted
    assert!(inspect_bytes(&b).is_ok());
    let mut b = qemu_bar();
    put(&mut b, 0x20, 0x0200_0002 | (1 << 8)); // next 4 bytes < protocol size 16
    assert_eq!(inspect_bytes(&b), Err(Error::InvalidCapability));
    let mut b = qemu_bar();
    put(&mut b, 0x20, 0); // capability ID 0
    assert_eq!(inspect_bytes(&b), Err(Error::InvalidCapability));
    let mut b = qemu_bar();
    put(&mut b, 16, 0x0004_7001); // xECP offset 0x10 inside the capability header
    assert_eq!(inspect_bytes(&b), Err(Error::InvalidCapability));
    let mut b = qemu_bar();
    put(&mut b, 16, 0x0010_7001); // xECP 0x40 overlaps the operational registers
    put(&mut b, 0x40, 0x0000_00c0); // unknown ID: reaches the overlap check
    assert_eq!(inspect_bytes(&b), Err(Error::InvalidCapability));
    let mut b = qemu_bar();
    put(&mut b, 0x30, 0x0300_0001 | 0); // no protocol after legacy rewrite
    put(&mut b, 0x20, 0x0000_0001);
    assert_eq!(inspect_bytes(&b), Err(Error::InvalidCapability)); // no protocol ports
    let mut b = qemu_bar();
    put(&mut b, 0x38, 0x0000_0904); // ports 4..12 beyond 8 ports
    assert_eq!(inspect_bytes(&b), Err(Error::InvalidCapability));
    let mut b = qemu_bar();
    put(&mut b, 0x38, 0x0000_0403); // USB3 3..6 overlaps USB2 1..4
    assert_eq!(inspect_bytes(&b), Err(Error::InvalidCapability));
    let mut b = qemu_bar();
    put(&mut b, 0x34, 0x2042_5354); // wrong protocol name
    assert_eq!(inspect_bytes(&b), Err(Error::Unsupported));
}

/// Chains of `n` headers placed at 0x3000 (clear of all register blocks).
fn chain(n: usize, terminal: bool) -> Vec<u8> {
    let mut b = qemu_bar();
    put(&mut b, 16, 0x0c00_7001);
    put(&mut b, 0x3000, 0x0200_0402);
    put(&mut b, 0x3004, 0x2042_5355);
    put(&mut b, 0x3008, 0x0000_0801);
    let mut at = 0x3010;
    for k in 1..n {
        let next = if k + 1 == n && terminal { 0 } else { 1 };
        put(&mut b, at, 0xc0 | (next << 8));
        at += 4;
    }
    b
}

#[test]
fn extended_chain_bound_and_progress() {
    assert_eq!(inspect_bytes(&chain(64, true)).unwrap().extended, 64);
    assert_eq!(inspect_bytes(&chain(65, true)), Err(Error::InvalidCapability));
    assert_eq!(inspect_bytes(&chain(70, false)), Err(Error::InvalidCapability));
    // Links are forward-only nonzero offsets: offsets strictly increase, so a
    // cycle is unrepresentable; a "loop" would need next == 0, which ends.
    let mut b = qemu_bar();
    put(&mut b, 0x20, 0x0200_0002); // protocol, next 0: terminal
    assert!(inspect_bytes(&b).is_ok());
}

#[test]
fn truncation_and_bounds() {
    let b = qemu_bar();
    assert_eq!(inspect_bytes(&b[..0x18]), Err(Error::Truncated)); // header itself
    assert_eq!(inspect_bytes(&b[..0x40]), Err(Error::Truncated)); // PAGESIZE at 0x48
    assert_eq!(inspect_bytes(&b[..0x2c]), Err(Error::Truncated)); // protocol body
    let mut b = qemu_bar();
    put(&mut b, 16, 0x0ffe_7001); // xECP 0x3ff8; 16-byte protocol crosses 16 KiB
    put(&mut b, 0x3ff8, 0x0200_0002);
    put(&mut b, 0x3ffc, 0x2042_5355);
    let mut longer = b.clone();
    longer.extend_from_slice(&[0; 16]);
    put(&mut longer, 0x4000, 0x0000_0801);
    assert_eq!(inspect_bytes(&longer), Err(Error::Bounds), "16 KiB extent, not slice length");
}

#[test]
fn controller_register_profile() {
    for (offset, value) in [
        (0usize, 0x0110_0040u32), // xHCI 1.1
        (0, 0x0120_0040),         // xHCI 1.2
        (4, 0x1100_1040),         // 17 ports
        (8, 0x0800_000f),         // scratchpads (low bits)
        (8, 0x0020_000f),         // scratchpads (high bits)
        (0x48, 2),                // no 4 KiB PAGESIZE
    ] {
        let mut b = qemu_bar();
        put(&mut b, offset, value);
        assert_eq!(inspect_bytes(&b), Err(Error::Unsupported), "{offset:#x} = {value:#x}");
    }
    let mut b = qemu_bar();
    put(&mut b, 0, 0x0100_0041); // CAPLENGTH not 8-aligned
    assert_eq!(inspect_bytes(&b), Err(Error::InvalidCapability));
    let mut b = qemu_bar();
    put(&mut b, 20, 0x2002); // misaligned doorbell offset
    assert_eq!(inspect_bytes(&b), Err(Error::Alignment));
    let mut b = qemu_bar();
    put(&mut b, 24, 0x4000); // runtime beyond the extent
    assert_eq!(inspect_bytes(&b), Err(Error::Bounds));
}

/// A refusing source models the C read budget and a failed register read.
struct Budget<'a> {
    bytes: &'a [u8],
    left: u32,
}
impl Words for Budget<'_> {
    fn word(&mut self, offset: u32) -> Result<u32, Error> {
        if self.left == 0 {
            return Err(Error::Budget);
        }
        self.left -= 1;
        Bytes(self.bytes).word(offset)
    }
}

#[test]
fn read_budget_and_reason_mapping() {
    let b = qemu_bar();
    let mut needed = 0;
    for left in 0..64 {
        match inspect(&mut Budget { bytes: &b, left }) {
            Ok(_) => {
                needed = left;
                break;
            }
            Err(e) => assert_eq!(e, Error::Budget),
        }
    }
    assert_eq!(needed, 14, "header 7 + PAGESIZE + 2 x (header, name, ports)");
    assert_eq!(Reason::from(Error::Truncated), Reason::ExtendedTruncated);
    assert_eq!(Reason::from(Error::InvalidCapability), Reason::ExtendedInvalid);
    assert_eq!(Reason::from(Error::Unsupported), Reason::ControllerProfile);
    assert_eq!(Reason::from(Error::Bounds), Reason::RegisterLayout);
    assert_eq!(Reason::from(Error::Budget), Reason::ReadBudget);
    assert_eq!(Reason::from(Error::Backend), Reason::RegisterRead);
    for r in [Reason::Qualified, Reason::StateChanged, Reason::Release, Reason::Profile] {
        let s = r.c_string();
        assert!(s.ends_with(b"\0") && !s[..s.len() - 1].contains(&0));
    }
}
