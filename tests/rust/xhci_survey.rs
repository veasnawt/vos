//! USB input bring-up, Increment 1: hosted tests for the read-only survey. The PCI tree and the xHCI register
//! file are fakes; the traits the survey uses have no write operation, so "never writes" is a property of the
//! types here plus the source scans at the end. Negative cases: absent and garbage devices, bridge cycles and
//! unassigned buses, the function and bus bounds, controllers beyond the recording limit, refused and truncated
//! register reads, malformed capability chains, hostile random register files (seeded), long output lines.
#![forbid(unsafe_code)]
use std::collections::BTreeMap;
use vos_xhci::xhci::survey::*;

// ---- fake PCI -----------------------------------------------------------------------------------------------------------

#[derive(Default)]
struct Tree {
    w: BTreeMap<(u8, u8, u8, u8), u32>,
    reads: u64,
}
impl Pci for Tree {
    fn read32(&mut self, bus: u8, dev: u8, func: u8, off: u8) -> u32 {
        assert_eq!(off % 4, 0, "unaligned configuration read");
        assert!(dev < 32 && func < 8);
        self.reads += 1;
        self.w.get(&(bus, dev, func, off)).copied().unwrap_or(0xffff_ffff)
    }
}
impl Tree {
    fn func(&mut self, b: u8, d: u8, f: u8, vendor: u16, device: u16, class: u32, header: u8) -> &mut Self {
        self.w.insert((b, d, f, 0), u32::from(device) << 16 | u32::from(vendor));
        self.w.insert((b, d, f, 4), 0x0010_0006);
        self.w.insert((b, d, f, 8), class << 8 | 0x21);
        self.w.insert((b, d, f, 0x0c), u32::from(header) << 16);
        for off in [0x10, 0x14, 0x2c, 0x34, 0x3c] {
            self.w.insert((b, d, f, off), 0);
        }
        self
    }
    fn set(&mut self, b: u8, d: u8, f: u8, off: u8, v: u32) -> &mut Self {
        self.w.insert((b, d, f, off), v);
        self
    }
    fn bridge(&mut self, b: u8, d: u8, secondary: u8, subordinate: u8) -> &mut Self {
        self.func(b, d, 0, 0x8086, 0x9d10, 0x060400, 1);
        self.set(b, d, 0, 0x18, u32::from(subordinate) << 16 | u32::from(secondary) << 8 | u32::from(b))
    }
}

const XHCI: u32 = 0x0c0330;

/// A laptop-like tree: host bridge, a PCH xHCI (multi-function), an EHCI, two nested bridges to an add-in xHCI, an
/// AMD-style IOMMU function on the last bus.
fn typical() -> Tree {
    let mut t = Tree::default();
    t.func(0, 0, 0, 0x8086, 0x5904, 0x060000, 0);
    t.func(0, 0x14, 0, 0x8086, 0x9d2f, XHCI, 0x80);
    t.set(0, 0x14, 0, 0x10, 0xdf22_0004).set(0, 0x14, 0, 0x14, 0).set(0, 0x14, 0, 0x2c, 0x5084_17aa).set(0, 0x14, 0, 0x3c, 0x0000_010b);
    t.func(0, 0x14, 2, 0x8086, 0x9d31, 0x118000, 0);
    t.func(0, 0x1d, 0, 0x8086, 0x1d2d, 0x0c0320, 0);
    t.bridge(0, 0x1c, 1, 3);
    t.bridge(1, 0, 2, 3);
    t.func(2, 0, 0, 0x1b21, 0x1142, XHCI, 0);
    t.set(2, 0, 0, 0x10, 0xfe40_0004);
    t.bridge(2, 1, 3, 3);
    t.func(3, 0, 0, 0x1022, 0x1419, 0x080600, 0);
    t
}

#[derive(Default)]
struct Capture(Vec<u8>);
impl Out for Capture {
    fn put(&mut self, text: &[u8]) {
        self.0.extend_from_slice(text);
    }
}
impl Capture {
    fn text(&self) -> String {
        String::from_utf8(self.0.clone()).unwrap()
    }
}

#[test]
fn the_scan_finds_controllers_on_bus_zero_and_behind_nested_bridges() {
    let mut t = typical();
    let s = scan(&mut t);
    assert_eq!((s.recorded, s.usb_total, s.buses, s.bridges, s.iommu_functions, s.skipped_bridges), (3, 3, 4, 3, 1, 0));
    assert!(!s.limit_hit);
    let [a, b, c] = [s.usb[0], s.usb[1], s.usb[2]];
    assert_eq!((a.bus, a.dev, a.func, a.vendor, a.device, a.is_xhci(), a.kind()), (0, 0x14, 0, 0x8086, 0x9d2f, true, "xhci"));
    assert_eq!((b.bus, b.dev, b.kind(), b.is_xhci()), (0, 0x1d, "ehci", false));
    assert_eq!((c.bus, c.dev, c.vendor, c.is_xhci()), (2, 0, 0x1b21, true), "behind two bridges");
    assert_eq!((a.subsystem, a.irq_line, a.irq_pin), (0x5084_17aa, 0x0b, 1));
    assert_eq!(a.bar_base(), Ok(0xdf22_0000));
    assert_eq!(c.bar_base(), Ok(0xfe40_0000));
    assert_eq!(s.seen, 9);
}

#[test]
fn absent_garbage_and_vendor_zero_functions_are_ignored() {
    let mut t = Tree::default();
    let s = scan(&mut t);
    assert_eq!((s.seen, s.recorded, s.buses), (0, 0, 1));
    // Vendor 0 is not a vendor; a 0xffff vendor is "nothing there".
    t.func(0, 3, 0, 0, 0x1234, XHCI, 0);
    t.func(0, 4, 0, 0xffff, 0x1234, XHCI, 0);
    assert_eq!(scan(&mut t).seen, 0);
    // A multi-function device whose functions 1-4 are absent still shows function 5.
    t.func(0, 5, 0, 0x1b36, 0x000d, XHCI, 0x80);
    t.func(0, 5, 5, 0x1b36, 0x000d, XHCI, 0);
    let s = scan(&mut t);
    assert_eq!((s.seen, s.recorded), (2, 2));
    assert_eq!((s.usb[1].dev, s.usb[1].func), (5, 5));
    // A single-function device does not have its higher functions probed.
    let mut t = Tree::default();
    t.func(0, 6, 0, 0x1b36, 0x000d, XHCI, 0);
    t.func(0, 6, 1, 0x1b36, 0x000d, XHCI, 0);
    assert_eq!(scan(&mut t).recorded, 1);
}

#[test]
fn bridge_cycles_unassigned_and_reversed_bus_numbers_are_not_followed() {
    // (secondary, subordinate) of a bridge on bus 1 that cannot be followed: unassigned, itself, reversed.
    for (sec, sub) in [(0u8, 0u8), (1, 1), (2, 1)] {
        let mut t = Tree::default();
        t.bridge(0, 1, 1, 1);
        t.bridge(1, 0, sec, sub);
        let s = scan(&mut t);
        assert_eq!((s.buses, s.skipped_bridges), (2, 1), "{sec}/{sub}");
    }
    // Two bridges naming the same secondary bus: it is scanned once and neither is counted as skipped.
    let mut t = Tree::default();
    t.bridge(0, 1, 1, 1).bridge(0, 2, 1, 1);
    t.func(1, 0, 0, 0x1b36, 0x000d, XHCI, 0);
    let s = scan(&mut t);
    assert_eq!((s.buses, s.recorded, s.skipped_bridges), (2, 1, 0));
    // A bridge on bus 1 claiming bus 0 cannot send the walk back.
    let mut t = Tree::default();
    t.bridge(0, 1, 1, 1).bridge(1, 0, 0, 0);
    assert_eq!(scan(&mut t).buses, 2);
}

#[test]
fn only_type_one_headers_are_bridges_and_only_upward_buses_are_followed() {
    // A function with the bridge class but a type-0 header is not a bridge: its bus-number dword is not read as one.
    let mut t = Tree::default();
    t.func(0, 1, 0, 0x8086, 0x1111, 0x060400, 0);
    t.set(0, 1, 0, 0x18, 0x0003_0100);
    t.func(1, 0, 0, 0x1b36, 0x000d, XHCI, 0);
    let s = scan(&mut t);
    assert_eq!((s.bridges, s.buses, s.recorded), (0, 1, 0));
    // A bridge on a reachable bus 5 that names the never-visited bus 4 is not followed (buses only go upward).
    let mut t = Tree::default();
    t.bridge(0, 1, 5, 5).bridge(5, 0, 4, 4);
    t.func(4, 0, 0, 0x1b36, 0x000d, XHCI, 0);
    let s = scan(&mut t);
    assert_eq!((s.buses, s.recorded, s.skipped_bridges), (2, 0, 1));
    // The same tree with the numbers in order finds the controller.
    let mut t = Tree::default();
    t.bridge(0, 1, 4, 5).bridge(4, 0, 5, 5);
    t.func(5, 0, 0, 0x1b36, 0x000d, XHCI, 0);
    assert_eq!(scan(&mut t).recorded, 1);
}

#[test]
fn the_bus_and_function_bounds_stop_the_walk() {
    // A chain of bridges longer than the queue: the walk ends, says so, and queues no more than the bound.
    let mut t = Tree::default();
    for b in 0..40u8 {
        t.bridge(b, 0, b + 1, 200);
    }
    let s = scan(&mut t);
    assert_eq!(usize::from(s.buses), BUS_LIMIT);
    assert!(s.limit_hit);
    // Fully populated multi-function buses end at the function limit.
    let mut t = Tree::default();
    for b in 0..20u8 {
        for d in 0..32u8 {
            t.func(b, d, 0, 0x1b36, 0x0001, 0x118000, 0x80);
            for f in 1..8u8 {
                t.func(b, d, f, 0x1b36, 0x0001, 0x118000, 0);
            }
        }
        if b < 19 {
            t.bridge(b, 31, b + 1, 200);
            t.set(b, 31, 0, 0x0c, 0x0081_0000);
        }
    }
    let s = scan(&mut t);
    assert_eq!(s.seen, FUNCTION_LIMIT);
    assert!(s.limit_hit);
    assert!(t.reads < 8 * u64::from(FUNCTION_LIMIT) + 4096, "{}", t.reads);
}

#[test]
fn more_controllers_than_the_recording_limit_are_counted_not_hidden() {
    let mut t = Tree::default();
    for d in 0..20u8 {
        t.func(0, d, 0, 0x1b36, 0x000d, XHCI, 0);
    }
    let s = scan(&mut t);
    assert_eq!((usize::from(s.recorded), s.usb_total), (MAX_USB, 20));
    let mut text = Capture::default();
    render_scan(&s, &mut text);
    assert!(text.text().contains("scan incomplete: controllers beyond 12 not listed"), "{}", text.text());
}

#[test]
fn bar_decoding_names_every_reason_a_controller_cannot_be_read() {
    let f = |bar0: u32, bar1: u32, command: u16, header: u8| UsbFn { bar0, bar1, command, header, ..Default::default() };
    assert_eq!(f(0xdf22_0004, 0, 6, 0).bar_base(), Ok(0xdf22_0000));
    assert_eq!(f(0x0000_0004, 1, 6, 0).bar_base(), Ok(0x1_0000_0000));
    assert_eq!(f(0xfeb0_0000, 0, 6, 0).bar_base(), Ok(0xfeb0_0000), "32-bit memory BAR");
    assert_eq!(f(0xdf22_0004, 0, 4, 0).bar_base(), Err("memory decoding off"));
    assert_eq!(f(0xdf22_0005, 0, 6, 0).bar_base(), Err("I/O BAR"));
    assert_eq!(f(0xdf22_0002, 0, 6, 0).bar_base(), Err("BAR type"));
    assert_eq!(f(0xdf22_0006, 0, 6, 0).bar_base(), Err("BAR type"));
    assert_eq!(f(0, 0, 6, 0).bar_base(), Err("BAR unassigned"));
    assert_eq!(f(0x0008_0000, 0, 6, 0).bar_base(), Err("BAR below 1 MiB"));
    assert_eq!(f(0xdf22_0804, 0, 6, 0).bar_base(), Err("BAR unaligned"));
    assert_eq!(f(0xdf22_0004, 0, 6, 1).bar_base(), Err("not a type-0 header"));
    assert_eq!(f(0xdf22_0004, 0, 6, 0x80).bar_base(), Ok(0xdf22_0000), "the multi-function bit alone is fine");
}

#[test]
fn pci_capabilities_are_summarised_and_malformed_lists_stop() {
    let mut t = typical();
    let f = scan(&mut t).usb[0];
    // PM at 0x50 (D3hot) -> MSI at 0x70 (enabled) -> PCIe at 0xa0 (No Snoop set) -> MSI-X at 0xb0 (disabled).
    t.set(0, 0x14, 0, 0x34, 0x50);
    t.set(0, 0x14, 0, 0x50, 0x0000_7001).set(0, 0x14, 0, 0x54, 3);
    t.set(0, 0x14, 0, 0x70, 0x0001_a005);
    t.set(0, 0x14, 0, 0xa0, 0x0000_b010).set(0, 0x14, 0, 0xa8, 0x0000_0800);
    t.set(0, 0x14, 0, 0xb0, 0x0000_0011);
    let i = pci_info(&mut t, &f);
    assert_eq!((i.pm, i.msi, i.pcie, i.msix, i.caps), (0x50, 0x70, 0xa0, 0xb0, 4));
    assert_eq!((i.power, i.msi_on, i.msix_on, i.devctl, i.truncated), (3, true, false, 0x0800, false));
    t.set(0, 0x14, 0, 0xb0, 0x8000_0011);
    assert!(pci_info(&mut t, &f).msix_on);
    // The last capability pointing back at the first is a loop: reported, not followed.
    t.set(0, 0x14, 0, 0xb0, 0x0000_5011);
    let i = pci_info(&mut t, &f);
    assert!(i.truncated && i.caps == 4, "{i:?}");
    // No capability list at all; a pointer inside the standard header; a PM capability at the very end of the space.
    let mut none = f;
    none.status = 0;
    assert_eq!(pci_info(&mut t, &none), PciInfo::default());
    t.set(0, 0x14, 0, 0x34, 0x10);
    assert_eq!(pci_info(&mut t, &f).caps, 0);
    t.set(0, 0x14, 0, 0x34, 0xfc).set(0, 0x14, 0, 0xfc, 0x0000_0001);
    let i = pci_info(&mut t, &f);
    assert_eq!((i.caps, i.pm), (1, 0), "a PM capability at 0xfc has no control word to read");
    // All 48 slots used, the last one ending the list: complete; pointing back at the first: truncated.
    for last in [0u32, 0x40] {
        let mut t = typical();
        for k in 0..48u32 {
            let next = if k == 47 { last } else { 0x40 + 4 * (k + 1) };
            t.set(0, 0x14, 0, (0x40 + 4 * k) as u8, 0x0000_0009 | next << 8);
        }
        t.set(0, 0x14, 0, 0x34, 0x40);
        let i = pci_info(&mut t, &f);
        assert_eq!((i.caps, i.truncated), (48, last != 0), "{last:#x}");
    }
}

// ---- fake xHCI registers ------------------------------------------------------------------------------------------------

#[derive(Default)]
struct Regs {
    w: BTreeMap<u32, u32>,
    reads: u32,
    fail_after: Option<u32>,
    log: Vec<u32>,
    /// A different value for the second read of a register (a controller that changes by itself).
    second: BTreeMap<u32, u32>,
}
impl Mmio for Regs {
    fn read32(&mut self, off: u32) -> Option<u32> {
        assert_eq!(off % 4, 0, "unaligned register read {off:#x}");
        assert!(off < WINDOW, "the survey asked the platform for {off:#x}, outside its own window");
        self.log.push(off);
        self.reads += 1;
        if self.fail_after.is_some_and(|k| self.reads > k) {
            return None;
        }
        if self.log.iter().filter(|o| **o == off).count() == 2
            && let Some(v) = self.second.get(&off)
        {
            return Some(*v);
        }
        Some(self.w.get(&off).copied().unwrap_or(0))
    }
}
impl Regs {
    fn set(&mut self, off: u32, v: u32) -> &mut Self {
        self.w.insert(off, v);
        self
    }
}

const USB_: u32 = u32::from_le_bytes(*b"USB ");

/// An Intel-style controller: CAPLENGTH 0x80, version 1.10, 32 slots, 8 interrupters, 12 ports, 5 scratchpads, AC64
/// and port power control, extended capabilities at 0x8000: legacy support (OS owned), USB 2.0 ports 1-8, USB 3.0
/// ports 9-12 and a Debug Capability.
fn intel() -> Regs {
    let mut r = Regs::default();
    r.set(0, 0x0110_0080).set(4, 12 << 24 | 8 << 8 | 32).set(8, 5 << 27).set(0xc, 0x0000_0a0a).set(0x10, 0x2000_0009);
    r.set(0x14, 0x3000).set(0x18, 0x2000).set(0x1c, 0);
    let op = 0x80;
    r.set(op, 8).set(op + 4, 1).set(op + 8, 1).set(op + 0x14, 2).set(op + 0x38, 0);
    r.set(op + 0x400 + 0x10 * 2, 0x0000_1203).set(op + 0x400 + 0x10 * 9, 0x0000_02a1);
    r.set(0x8000, 0x0100_0401).set(0x8004, 1);
    r.set(0x8010, 0x0200_0402).set(0x8014, USB_).set(0x8018, 8 << 8 | 1);
    r.set(0x8020, 0x0300_0602).set(0x8024, USB_).set(0x8028, 4 << 8 | 9);
    r.set(0x8038, 0x0000_000a);
    r
}

#[test]
fn an_intel_style_controller_is_decoded_completely() {
    let mut r = intel();
    let x = inspect_xhci(&mut r);
    assert_eq!(x.stopped, None, "{x:?}");
    assert_eq!((x.version, x.caplength, x.slots(), x.interrupters(), x.ports, x.scratchpads()), (0x0110, 0x80, 32, 8, 12, 5));
    assert_eq!((x.usbcmd, x.usbsts, x.pagesize, x.dnctrl, x.usbcmd2, x.usbsts2), (8, 1, 1, 2, 8, 1));
    assert_eq!((x.ports_read, x.connected), (12, 2));
    assert_eq!((x.conn[0].index, x.conn[0].portsc, x.conn[1].index, x.conn[1].portsc), (2, 0x1203, 9, 0x02a1));
    assert_eq!(x.caps, 4);
    assert_eq!(x.ids[..4], [1, 2, 2, 10]);
    assert_eq!(x.legacy, Some((0x8000, 0x0100_0401, 1)));
    assert_eq!(x.dbc, Some((0x8038, 0)));
    assert_eq!(
        (x.protocols, x.proto[0], x.proto[1]),
        (2, Proto { major: 2, minor: 0, first: 1, count: 8 }, Proto { major: 3, minor: 0, first: 9, count: 4 })
    );
    assert!(r.reads <= u32::from(READ_LIMIT));
}

#[test]
fn a_controller_that_changes_while_it_is_read_is_flagged() {
    let mut r = intel();
    let x = inspect_xhci(&mut r);
    let mut out = Capture::default();
    render_xhci(&scan(&mut typical()).usb[0], &x, &mut out);
    assert!(!out.text().contains("WARNING"));
    let mut y = x;
    y.usbsts2 ^= 1;
    let mut out = Capture::default();
    render_xhci(&scan(&mut typical()).usb[0], &y, &mut out);
    assert!(out.text().contains("WARNING: usbcmd/usbsts changed by themselves between two reads"), "{}", out.text());
}

#[test]
fn scratchpad_counts_combine_the_high_and_low_fields() {
    for n in [0u32, 1, 4, 5, 31, 32, 63, 64, 65, 1023] {
        let mut r = intel();
        r.set(8, ((n >> 5) & 31) << 21 | (n & 31) << 27);
        assert_eq!(inspect_xhci(&mut r).scratchpads(), n);
    }
}

#[test]
fn a_controller_that_reads_as_all_ones_or_has_a_bad_caplength_is_reported_not_trusted() {
    let mut r = Regs::default();
    r.set(0, 0xffff_ffff);
    let x = inspect_xhci(&mut r);
    assert_eq!(x.stopped, Some("reads as all ones"));
    assert_eq!(r.reads, 1);
    for cap in [0u32, 4, 0x1c, 0x21, 0x2a, 0xff] {
        let mut r = intel();
        r.set(0, 0x0110_0000 | cap);
        let x = inspect_xhci(&mut r);
        assert_eq!(x.stopped, Some("bad CAPLENGTH"), "{cap:#x}");
        assert_eq!(x.usbcmd, 0, "no operational register is read through a bad CAPLENGTH");
        assert!(r.log.iter().all(|o| *o < 0x20), "{:?}", r.log);
    }
}

#[test]
fn a_refused_or_truncated_read_ends_the_walk_with_what_was_read() {
    let full = {
        let mut r = intel();
        let x = inspect_xhci(&mut r);
        assert_eq!(x.stopped, None);
        r.reads
    };
    assert!(full > 30, "{full}");
    // The nine ring-pointer reads come just before the second USBCMD/USBSTS reading. A refusal among them is recorded apart
    // (`ptr_stopped`) and the walk still tries the second USBCMD once, so a controller whose runtime registers are out of
    // reach still gets its stability check; it is refused too, which ends the walk one read later.
    let pointer_block = full - 11..full - 2;
    for k in 0..full {
        let mut r = intel();
        r.fail_after = Some(k);
        let x = inspect_xhci(&mut r);
        assert!(x.stopped.is_some(), "fail_after {k}: a refused read must be reported: {x:?}");
        assert_eq!(x.stopped, Some("read refused"));
        assert!(x.reads <= READ_LIMIT);
        if pointer_block.contains(&k) {
            assert_eq!((r.reads, x.ptr_stopped), (k + 2, Some("read refused")), "fail_after {k}");
            assert_eq!((x.usbcmd2, x.usbsts2), (0, 0));
        } else {
            assert_eq!(r.reads, k + 1, "the walk stops at the first refusal");
            assert_eq!(x.ptr_stopped, None, "fail_after {k}");
        }
    }
}

#[test]
fn a_huge_port_count_is_bounded_and_registers_past_the_window_are_not_asked_for() {
    let mut r = intel();
    r.set(4, 255 << 24 | 8 << 8 | 32);
    let x = inspect_xhci(&mut r);
    assert_eq!((x.ports, x.ports_read), (255, PORT_LIMIT), "{x:?}");
    // CAPLENGTH 0xfc puts the port registers at 0x4fc; port 64 is still inside the 64 KiB window, so the bound is the count.
    let mut r = intel();
    r.set(4, 255 << 24 | 8 << 8 | 32).set(0, 0x0110_00fc);
    let x = inspect_xhci(&mut r);
    assert_eq!(x.ports_read, PORT_LIMIT);
    assert!(r.log.iter().all(|o| *o < WINDOW));
}

#[test]
fn capability_chains_that_run_on_or_off_the_window_are_cut() {
    // A chain of forty one-dword capabilities exceeds the limit.
    let mut r = Regs::default();
    r.set(0, 0x0110_0020).set(4, 1 << 24 | 1 << 8 | 1).set(0x10, 0x2000 << 16);
    for k in 0..40u32 {
        r.set(0x8000 + 4 * k, 0x0000_0103);
    }
    let x = inspect_xhci(&mut r);
    assert_eq!((usize::from(x.caps), x.stopped), (CAP_LIMIT, Some("capability list too long")));
    // A first pointer past the window ends with a window refusal and no capability.
    let mut r = Regs::default();
    r.set(0, 0x0110_0020).set(4, 1 << 24 | 1 << 8 | 1).set(0x10, 0xffff << 16);
    let x = inspect_xhci(&mut r);
    assert_eq!((x.caps, x.stopped), (0, Some("outside window")));
    // The last dword of the window is readable; the first dword past it is never even requested.
    let mut r = Regs::default();
    r.set(0, 0x0110_0020).set(4, 1 << 24 | 1 << 8 | 1).set(0x10, 0x3fff << 16);
    r.set(0xfffc, 0x0000_0003);
    let x = inspect_xhci(&mut r);
    assert_eq!((x.caps, x.stopped), (1, None));
    let mut r = Regs::default();
    r.set(0, 0x0110_0020).set(4, 1 << 24 | 1 << 8 | 1).set(0x10, 0x4000 << 16);
    let x = inspect_xhci(&mut r);
    assert_eq!((x.caps, x.stopped), (0, Some("outside window")));
    assert!(r.log.iter().all(|o| *o < WINDOW));
    // A last capability whose next pointer leaves the window ends there, with the capability kept.
    let mut r = Regs::default();
    r.set(0, 0x0110_0020).set(4, 1 << 24 | 1 << 8 | 1).set(0x10, 0x3ffc << 16);
    r.set(0xfff0, 0x0000_ff09);
    let x = inspect_xhci(&mut r);
    assert_eq!((x.caps, x.stopped), (1, Some("outside window")));
    // No extended capabilities at all is a normal, complete result.
    let mut r = Regs::default();
    r.set(0, 0x0100_0020).set(4, 1 << 24 | 1 << 8 | 1);
    let x = inspect_xhci(&mut r);
    assert_eq!((x.caps, x.stopped, x.legacy, x.dbc), (0, None, None, None));
}

#[test]
fn legacy_debug_and_protocol_capabilities_are_recorded_once_and_by_name() {
    fn base() -> Regs {
        let mut r = Regs::default();
        r.set(0, 0x0110_0020).set(4, 1 << 24 | 1 << 8 | 1).set(0x10, 0x2000 << 16);
        r
    }
    // Two legacy-support capabilities: the first one is the one that is reported.
    let mut r = base();
    r.set(0x8000, 0x0100_0401).set(0x8004, 0x11).set(0x8010, 0x0001_0001).set(0x8014, 0x22);
    let x = inspect_xhci(&mut r);
    assert_eq!((x.legacy, x.caps, x.stopped), (Some((0x8000, 0x0100_0401, 0x11)), 2, None));
    // Two Debug Capabilities: likewise.
    let mut r = base();
    r.set(0x8000, 0x0000_0c0a).set(0x8020, 0x8000_0001).set(0x8030, 0x0000_000a).set(0x8050, 0x1234);
    let x = inspect_xhci(&mut r);
    assert_eq!((x.dbc, x.caps, x.stopped), (Some((0x8000, 0x8000_0001)), 2, None));
    // A protocol capability that is not named "USB " is a capability but not a listed protocol.
    let mut r = intel();
    r.set(0x8014, u32::from_le_bytes(*b"XYZ ")).set(0x8024, u32::from_le_bytes(*b"XYZ "));
    let x = inspect_xhci(&mut r);
    assert_eq!((x.caps, x.protocols), (4, 0));
    // More protocols than the table holds: the extra ones are counted as capabilities, not stored.
    let mut r = base();
    for k in 0..8u32 {
        r.set(0x8000 + 16 * k, if k == 7 { 0x0300_0002 } else { 0x0300_0402 });
        r.set(0x8000 + 16 * k + 4, USB_).set(0x8000 + 16 * k + 8, (k + 1) | 1 << 8);
    }
    let x = inspect_xhci(&mut r);
    assert_eq!((x.caps, x.protocols), (8, 6));
    assert_eq!(x.proto[5], Proto { major: 3, minor: 0, first: 6, count: 1 });
}

#[test]
fn a_controller_that_changes_by_itself_between_two_reads_is_caught_by_the_reader() {
    for (reg, value) in [(0x80u32, 1u32), (0x84, 9)] {
        let mut r = intel();
        r.second.insert(reg, value);
        let x = inspect_xhci(&mut r);
        assert_ne!((x.usbcmd, x.usbsts), (x.usbcmd2, x.usbsts2), "{reg:#x}");
        let mut out = Capture::default();
        render_xhci(&scan(&mut typical()).usb[0], &x, &mut out);
        assert!(out.text().contains("WARNING: usbcmd/usbsts changed by themselves"), "{}", out.text());
    }
    let mut r = intel();
    let x = inspect_xhci(&mut r);
    assert_eq!((x.usbcmd, x.usbsts), (x.usbcmd2, x.usbsts2));
}

// ---- rendering -----------------------------------------------------------------------------------------------------------

#[test]
fn the_report_lines_are_short_printable_and_complete() {
    let mut t = typical();
    let s = scan(&mut t);
    let f = s.usb[0];
    let mut out = Capture::default();
    render_scan(&s, &mut out);
    render_pci(&f, &pci_info(&mut t, &f), &f.bar_base(), &mut out);
    let x = inspect_xhci(&mut intel());
    render_xhci(&f, &x, &mut out);
    render_note("example note", &mut out);
    let text = out.text();
    for line in text.lines() {
        assert!(line.len() <= 118, "{} bytes: {line}", line.len());
        assert!(line.starts_with("usb: "), "{line}");
        assert!(line.bytes().all(|b| (0x20..0x7f).contains(&b)), "{line}");
    }
    assert!(text.ends_with("\r\n") && !text.contains("\n\n"));
    for want in [
        "usb: survey (read-only) pci functions 9 buses 4 bridges 3 usb 3 iommu-fn 1",
        "usb: xhci 00:14.0 0x8086:0x9d2f cls 0x0c0330 rev 0x21 sub 0x508417aa cmd 0x0006",
        "usb: ehci 00:1d.0 ",
        "usb: xhci 02:00.0 0x1b21:0x1142 ",
        "usb: 00:14.0 bar0 0xdf220000 irq 11/pin1 pm D0",
        "usb: 00:14.0 hci 0x0110 caplen 0x80 slots 32 intr 8 ports 12 scratchpads 5 ac64 1 csz 0 ppc 1",
        "usbcmd 0x00000008 usbsts 0x00000001 (R/S 0 HCH 1 HSE 0 HCE 0)",
        "ports read 12 connected 2 p3=0x00001203 p10=0x000002a1",
        "ext caps 4 ids 1 2 2 10 legacy@0x8000 bios 0 os 1 smi 0x00000001",
        "protocols usb2.00 ports 1+8 usb3.00 ports 9+4",
        "usb: example note",
    ] {
        assert!(text.contains(want), "missing {want:?} in\n{text}");
    }
}

#[test]
fn the_summary_fits_an_80_column_display_and_says_when_nothing_was_read() {
    let f = scan(&mut typical()).usb[0];
    let x = inspect_xhci(&mut intel());
    let mut out = Capture::default();
    render_summary(&f, &x, &mut out);
    let text = out.text();
    assert!(text.lines().all(|l| l.len() <= 80), "{text}");
    assert!(text.contains("usb: SUMMARY xhci 00:14.0 8086:9d2f v0110 ports 12 connected 2 slots 32 spad 5"), "{text}");
    assert!(text.contains("usb:   bios-owned 0 os-owned 1 running 0 halted 1 debug-cap"), "{text}");
    let mut out = Capture::default();
    render_summary(&f, &Xhci { stopped: Some("BAR unusable"), ..Default::default() }, &mut out);
    assert_eq!(out.text(), "usb: SUMMARY xhci 00:14.0 8086:9d2f NOT READ\r\n");
    let mut r = intel();
    r.fail_after = Some(20);
    let mut out = Capture::default();
    render_summary(&f, &inspect_xhci(&mut r), &mut out);
    assert!(out.text().contains("STOPPED: read refused"), "{}", out.text());
    let mut r = Regs::default();
    r.set(0, 0x0100_0020).set(4, 1 << 24 | 1 << 8 | 1);
    let mut out = Capture::default();
    render_summary(&f, &inspect_xhci(&mut r), &mut out);
    assert!(out.text().contains("no legacy-handoff capability"), "{}", out.text());
}

#[test]
fn a_walk_that_stopped_early_says_where_and_why() {
    let mut r = intel();
    r.fail_after = Some(10);
    let x = inspect_xhci(&mut r);
    let mut out = Capture::default();
    render_xhci(&scan(&mut typical()).usb[0], &x, &mut out);
    assert!(out.text().contains("walk stopped: read refused after 11 reads"), "{}", out.text());
}

#[test]
fn unprintable_bytes_and_overlong_lines_are_neutralised() {
    let mut l = Line::new();
    l.s("a\u{1}b\u{7f}c\n");
    assert_eq!(l.bytes(), b"a?b?c?");
    let mut l = Line::new();
    for _ in 0..500 {
        l.s("0123456789");
    }
    let mut out = Capture::default();
    l.flush(&mut out);
    assert_eq!(out.0.len(), 120);
    assert!(out.0.ends_with(b"\r\n"));
    let mut l = Line::new();
    l.x(0, 0).s(" ").x(0, 4).s(" ").x(u64::MAX, 8).s(" ").x(0xabc, 2).s(" ").hx(0xabc, 5).s(" ").d(0).s(" ").d(u64::MAX);
    assert_eq!(std::str::from_utf8(l.bytes()).unwrap(), "0x0 0x0000 0xffffffffffffffff 0xabc 00abc 0 18446744073709551615");
}

// ---- hostile input ---------------------------------------------------------------------------------------------------------

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn word(&mut self) -> u32 {
        match self.next() % 8 {
            0 => 0,
            1 => 0xffff_ffff,
            2 => 0x0110_0000 | (self.next() % 0x120) as u32,
            _ => self.next() as u32,
        }
    }
}
struct Hash {
    seed: u64,
    reads: u32,
}
impl Mmio for Hash {
    fn read32(&mut self, off: u32) -> Option<u32> {
        assert!(off % 4 == 0 && off < WINDOW);
        self.reads += 1;
        let mut r = Rng(self.seed ^ (u64::from(off) + 1).wrapping_mul(0x9e37_79b9_7f4a_7c15) | 1);
        // keep the header plausible so that the walk gets deep as often as it stops early
        Some(match off {
            0 if r.next() % 4 != 0 => 0x0110_0040 + (r.next() % 3) as u32 * 0x20,
            0x10 if r.next() % 2 == 0 => (r.next() as u32 % 0x2000) << 16 | 0x8000_0000 >> 31,
            _ => r.word(),
        })
    }
}

#[test]
fn random_register_files_never_panic_and_never_exceed_the_read_budget() {
    let mut deep = 0;
    for seed in 1..=20_000u64 {
        let mut m = Hash { seed, reads: 0 };
        let x = inspect_xhci(&mut m);
        assert!(m.reads <= u32::from(READ_LIMIT), "seed {seed}: {} reads", m.reads);
        assert_eq!(u32::from(x.reads), m.reads);
        assert!(usize::from(x.caps) <= CAP_LIMIT && usize::from(x.protocols) <= x.proto.len() && x.connected <= x.ports_read);
        deep += u32::from(x.caps > 0);
        let mut out = Capture::default();
        render_xhci(&UsbFn::default(), &x, &mut out);
        assert!(out.text().lines().all(|l| l.len() <= 118));
    }
    assert!(deep > 100, "the generator reached capability chains in only {deep} runs");
}

struct Noise(u64);
impl Pci for Noise {
    fn read32(&mut self, bus: u8, dev: u8, func: u8, off: u8) -> u32 {
        let mut r = Rng(self.0 ^ (u64::from(bus) << 24 | u64::from(dev) << 16 | u64::from(func) << 8 | u64::from(off)).wrapping_mul(0x9e37_79b9_7f4a_7c15) | 1);
        match (off, r.next() % 3) {
            (0, 0) => 0xffff_ffff,
            (0, _) => 0x8086_0000 | (r.next() as u32 & 0xffff) | 1,
            (8, _) => [0x0c03_3000, 0x0604_0000, 0x0806_0000, 0x0c03_2000, r.next() as u32][(r.next() % 5) as usize],
            (0x0c, _) => (r.next() as u32 % 3) << 22 | (r.next() as u32 & 0xff) << 16,
            (0x18, _) => r.next() as u32,
            _ => r.next() as u32,
        }
    }
}

#[test]
fn random_pci_trees_terminate_within_the_bounds() {
    for seed in 1..=300u64 {
        let s = scan(&mut Noise(seed));
        assert!(s.seen <= FUNCTION_LIMIT && usize::from(s.buses) <= BUS_LIMIT && usize::from(s.recorded) <= MAX_USB, "seed {seed}");
        assert!(u16::from(s.recorded) <= s.usb_total);
        for f in s.usb.iter().take(usize::from(s.recorded)) {
            let _ = f.bar_base();
            let _ = pci_info(&mut Noise(seed), f);
        }
        let mut out = Capture::default();
        render_scan(&s, &mut out);
        assert!(out.text().lines().all(|l| l.len() <= 118));
    }
}

// ---- no write path ------------------------------------------------------------------------------------------------------------

#[test]
fn the_platform_sources_contain_no_pci_or_port_or_register_write() {
    let c = include_str!("../../arch/x86_64/xhci_survey.c");
    for bad in ["pci_write", "outb", "outw", "outl", "wrmsr", "write32", "write16", "write8", "write_cr", "cpu_enable_interrupts"] {
        assert!(!c.contains(bad), "xhci_survey.c mentions {bad}");
    }
    let rs = include_str!("../../arch/x86_64/rust/xhci_survey.rs");
    for bad in ["write_volatile", "write_unaligned", "ptr::write", ".write(", "pci_write", "asm!"] {
        assert!(!rs.contains(bad), "xhci_survey.rs mentions {bad}");
    }
    let hosted = include_str!("../../kernel/rust/usb/xhci/survey.rs");
    assert!(!hosted.contains("fn write") && !hosted.contains("unsafe"), "the hosted module has no write method and no unsafe code");
    // The only memory operations the survey trait objects have are reads.
    assert!(hosted.contains("pub trait Pci {\n    fn read32") && hosted.contains("pub trait Mmio {\n    fn read32"));
}

// ---- Increment 2: 21 ports, ring pointers ------------------------------------------------------------------------------------

/// The Dell's controller: 21 ports, all disconnected, powered off by default (PORTSC 0x2a0 is "disconnected, powered, no change").
fn dell() -> Regs {
    let mut r = intel();
    r.set(4, 21 << 24 | 8 << 8 | 32);
    for p in 0..21u32 {
        r.set(0x80 + 0x400 + 0x10 * p, 0x0000_02a0);
    }
    r.w.remove(&(0x80 + 0x400 + 0x10 * 2));
    r.w.remove(&(0x80 + 0x400 + 0x10 * 9));
    r.set(0x80 + 0x400 + 0x10 * 2, 0x0000_02a0).set(0x80 + 0x400 + 0x10 * 9, 0x0000_02a0);
    r
}

#[test]
fn all_twenty_one_ports_are_read_and_a_connection_on_the_last_one_is_seen() {
    let mut r = dell();
    let x = inspect_xhci(&mut r);
    assert_eq!((x.stopped, x.ports, x.ports_read, x.connected), (None, 21, 21, 0), "{x:?}");
    assert!(x.portsc[..21].iter().all(|sc| *sc == 0x2a0));
    // the 21st port (index 20) is the last one: its register is the 21st PORTSC
    let mut r = dell();
    r.set(0x80 + 0x400 + 0x10 * 20, 0x0000_0201);
    let x = inspect_xhci(&mut r);
    assert_eq!((x.connected, x.conn[0].index, x.conn[0].portsc), (1, 20, 0x0000_0201));
    assert!(r.log.contains(&(0x80 + 0x400 + 0x10 * 20)) && !r.log.contains(&(0x80 + 0x400 + 0x10 * 21)));
    let mut out = Capture::default();
    render_xhci_ports(&x, &mut out);
    let text = out.text();
    assert!(text.contains("portsc of 21 of 21 ports: connected 1 enabled 0 powered 21 over-current 0"), "{text}");
    assert!(text.contains(" p21=00000201") && text.contains(" p1=000002a0 "), "{text}");
    assert!(text.lines().all(|l| l.len() <= 118), "{text}");
}

#[test]
fn the_port_summary_counts_each_status_bit_on_its_own() {
    let x = Xhci { ports: 8, ports_read: 8, portsc: { let mut a = [0u32; 32]; a[..5].copy_from_slice(&[0x1, 0x203, 0x8, 0x200, 0x0000_0008 | 0x200]); a }, ..Default::default() };
    let mut out = Capture::default();
    render_xhci_ports(&x, &mut out);
    let text = out.text();
    assert!(text.contains("portsc of 8 of 8 ports: connected 2 enabled 1 powered 3 over-current 2"), "{text}");
    assert!(text.contains(" p1=00000001 p2=00000203 p3=00000008 p4=00000200 p5=00000208 p6=00000000"), "{text}");
}

#[test]
fn more_ports_than_the_recorded_thirty_two_are_counted_but_only_thirty_two_are_shown() {
    let mut r = intel();
    r.set(4, 40 << 24 | 8 << 8 | 32);
    for p in 0..40u32 {
        r.set(0x80 + 0x400 + 0x10 * p, 1);
    }
    let x = inspect_xhci(&mut r);
    assert_eq!((x.ports, x.ports_read, x.connected), (40, 40, 40));
    let mut out = Capture::default();
    render_xhci_ports(&x, &mut out);
    let text = out.text();
    assert!(text.contains("portsc of 32 of 40 ports: connected 32") && text.contains("(first 32 shown)"), "{text}");
    assert!(text.contains(" p32=") && !text.contains(" p33="), "{text}");
    assert!(text.lines().all(|l| l.len() <= 118), "{text}");
}

#[test]
fn ring_pointers_are_decoded_from_their_registers_and_never_dereferenced() {
    let mut r = intel();
    let rt = 0x2000;
    r.set(0x80 + 0x18, 0x0000_0009).set(0x80 + 0x30, 0x7f00_1000).set(0x80 + 0x34, 0x1);
    r.set(rt + 0x20, 0x2).set(rt + 0x28, 1).set(rt + 0x30, 0x7f00_2000).set(rt + 0x34, 0).set(rt + 0x38, 0x7f00_3008).set(rt + 0x3c, 0);
    let x = inspect_xhci(&mut r);
    assert_eq!(x.ptr_stopped, None);
    assert_eq!((x.dcbaap, x.crcr, x.ir_iman, x.ir_erstsz, x.ir_erstba, x.ir_erdp), (0x1_7f00_1000, 9, 2, 1, 0x7f00_2000, 0x7f00_3008));
    let reads: Vec<u32> = r.log.iter().copied().filter(|o| (0x2000..0x3000).contains(o)).collect();
    assert_eq!(reads, vec![rt + 0x20, rt + 0x28, rt + 0x30, rt + 0x34, rt + 0x38, rt + 0x3c], "exactly interrupter 0, nothing else");
    let mut out = Capture::default();
    let f = scan(&mut typical()).usb[0];
    render_pointers(&f, &x, &|a| if a >> 24 == 0x7f { "ram" } else { "none" }, &mut out);
    let text = out.text();
    assert!(text.contains("dcbaap 0x17f001000 [none] crcr(lo) 0x00000009 ir0 erstba 0x7f002000 [ram] erdp 0x7f003008 [ram]"), "{text}");
    assert!(text.lines().all(|l| l.len() <= 118), "{text}");
}

#[test]
fn unusable_runtime_offsets_are_recorded_and_the_rest_of_the_walk_still_runs() {
    for (rtsoff, why) in [(0x2004u32, "RTSOFF unaligned"), (0xffff_ffe0, "RTSOFF overflows"), (0x0fff_ffe0, "register outside the MMIO window")] {
        let mut r = intel();
        r.set(0x18, rtsoff);
        let x = inspect_xhci(&mut r);
        assert_eq!((x.stopped, x.ptr_stopped.is_some()), (None, true), "{rtsoff:#x}: {x:?}");
        if why != "register outside the MMIO window" {
            assert_eq!(x.ptr_stopped, Some(why), "{rtsoff:#x}");
        }
        assert_eq!((x.usbcmd2, x.usbsts2), (8, 1), "the stability reading is still taken");
        assert!(r.log.iter().all(|o| *o < WINDOW));
        let mut out = Capture::default();
        render_pointers(&scan(&mut typical()).usb[0], &x, &|_| "x", &mut out);
        assert!(out.text().contains("ring pointers not fully read"), "{}", out.text());
    }
}

// ---- Increment 2: Intel port routing -----------------------------------------------------------------------------------------

struct Spy<'a>(&'a mut Tree, Vec<u8>);
impl Pci for Spy<'_> {
    fn read32(&mut self, bus: u8, dev: u8, func: u8, off: u8) -> u32 {
        self.1.push(off);
        self.0.read32(bus, dev, func, off)
    }
}

fn dell_pci() -> Tree {
    let mut t = Tree::default();
    t.func(0, 0x14, 0, 0x8086, 0x8c31, XHCI, 0x80);
    t.set(0, 0x14, 0, 0x10, 0xf7c2_0004);
    t.func(0, 0x1a, 0, 0x8086, 0x8c2d, 0x0c0320, 0x80);
    t.set(0, 0x1a, 0, 0x10, 0xf7c3_1c00);
    t.func(0, 0x1d, 0, 0x8086, 0x8c26, 0x0c0320, 0x80);
    t.set(0, 0x1d, 0, 0x10, 0xf7c3_0800);
    t
}

#[test]
fn intel_routing_registers_are_read_and_interpreted_without_assuming_a_port_layout() {
    let mut t = dell_pci();
    t.set(0, 0x14, 0, 0xd0, 0x0000_0000).set(0, 0x14, 0, 0xd4, 0x0000_3fff).set(0, 0x14, 0, 0xd8, 0x0000_0000).set(0, 0x14, 0, 0xdc, 0x0000_000f);
    let s = scan(&mut t);
    let f = s.usb[0];
    let mut spy = Spy(&mut t, vec![]);
    let r = intel_routing(&mut spy, &f).unwrap();
    assert_eq!(spy.1, vec![0xd0, 0xd4, 0xd8, 0xdc], "exactly the four routing registers, each read once");
    assert_eq!((r.usb2_routable(), r.usb2_to_xhci(), r.usb2_to_ehci()), (0x3fff, 0, 0x3fff), "nothing routed: every port stays with its EHCI");
    assert_eq!((r.usb3_enabled(), r.usb3_disabled()), (0, 0xf));
    let mut out = Capture::default();
    render_routing(&f, &r, &mut out);
    let text = out.text();
    assert!(text.contains("xusb2pr 0x00000000 xusb2prm 0x00003fff\r\n"), "{text}");
    assert!(text.contains("to xhci -, left with ehci 1-14"), "{text}");
    assert!(text.contains("superspeed ports enabled -\r\n") && text.contains("superspeed ports disabled 1-4  (bit n"), "{text}");
    assert!(text.contains("usb:   usb3_pssen 0x00000000 usb3prm 0x0000000f"), "{text}");
    assert!(text.lines().all(|l| l.len() <= 118), "{text}");
    // partly routed: ports 1, 2, 5 to the xHCI; a bit outside the routable mask is not a routed port
    let mut t = dell_pci();
    t.set(0, 0x14, 0, 0xd0, 0x8000_4013).set(0, 0x14, 0, 0xd4, 0x0000_3fff).set(0, 0x14, 0, 0xd8, 0x0000_001f).set(0, 0x14, 0, 0xdc, 0x0000_000f);
    let f = scan(&mut t).usb[0];
    let r = intel_routing(&mut t, &f).unwrap();
    assert_eq!((r.usb2_to_xhci(), r.usb2_to_ehci(), r.usb3_enabled()), (0x13, 0x3fec, 0xf), "bits outside the routable mask are neither routed nor left");
    // a routable mask wider than the PCH has ports is cut to the fifteen it has
    let wide = Routing { xusb2pr: 0x8000_4013, xusb2prm: u32::MAX, usb3_pssen: 0, usb3prm: 0 };
    assert_eq!((wide.usb2_routable(), wide.usb2_to_xhci(), wide.usb2_to_ehci()), (ROUTING_PORT_MASK, 0x4013, 0x3fec));
    let mut out = Capture::default();
    render_routing(&f, &r, &mut out);
    assert!(out.text().contains("to xhci 1-2,5, left with ehci 3-4,6-14"), "{}", out.text());
}

#[test]
fn routing_is_only_read_for_an_intel_xhci_type_zero_header_that_has_the_registers() {
    let mut t = dell_pci();
    for off in [0xd0, 0xd4, 0xd8, 0xdc] {
        t.set(0, 0x14, 0, off, 0);
    }
    let s = scan(&mut t);
    let (xhci, ehci) = (s.usb[0], s.usb[1]);
    assert!(ehci.is_ehci());
    // zero is a valid answer; only all ones means "no such registers"
    assert!(intel_routing(&mut t, &xhci).is_some());
    for (f, why) in [
        (ehci, "an EHCI has no routing registers"),
        (UsbFn { vendor: 0x1b36, ..xhci }, "another vendor"),
        (UsbFn { header: 1, ..xhci }, "a bridge header"),
    ] {
        let mut spy = Spy(&mut t, vec![]);
        assert_eq!(intel_routing(&mut spy, &f), None, "{why}");
        assert!(spy.1.is_empty(), "{why}: no configuration read at all");
    }
    // a function that does not decode the registers reads all ones everywhere
    for off in [0xd0, 0xd4, 0xd8, 0xdc] {
        t.set(0, 0x14, 0, off, 0xffff_ffff);
    }
    assert_eq!(intel_routing(&mut t, &xhci), None);
    // one register readable is enough to report
    t.set(0, 0x14, 0, 0xd4, 0x3fff);
    assert!(intel_routing(&mut t, &xhci).is_some());
}

// ---- Increment 2: EHCI -----------------------------------------------------------------------------------------------------

/// An Intel PCH EHCI: CAPLENGTH 0x20, version 1.0, six ports (two companions of three), EECP 0x68, halted... running.
fn ehci_regs() -> Regs {
    let mut r = Regs::default();
    r.set(0, 0x0100_0020).set(4, 2 << 12 | 3 << 8 | 6).set(8, 0x68 << 8);
    let op = 0x20;
    r.set(op, 0x0001_0001).set(op + 4, 0x0000_0000).set(op + 0x40, 1);
    r.set(op + 0x44, 0x0000_1005); // port 1: connected, enabled, high speed (bit 2 = enabled; line status bits are not used), powered
    r.set(op + 0x48, 0x0000_1000); // port 2: powered, empty
    r.set(op + 0x4c, 0x0000_3001); // port 3: connected, owned by the companion
    r
}
fn ehci_pci() -> (Tree, UsbFn) {
    let mut t = dell_pci();
    t.set(0, 0x1a, 0, 0x68, 0x0000_0001 | 1 << 16).set(0, 0x1a, 0, 0x6c, 0x0000_0001);
    let f = scan(&mut t).usb[1];
    (t, f)
}

#[test]
fn an_ehci_is_decoded_with_its_ports_and_legacy_support() {
    let (mut t, f) = ehci_pci();
    assert!(f.is_ehci() && !f.is_xhci());
    assert_eq!(f.ehci_base(), Ok(0xf7c3_1c00), "an EHCI BAR may sit at a 1 KiB boundary");
    assert_eq!(f.bar_base(), Err("BAR unaligned"), "an xHCI BAR may not");
    let mut r = ehci_regs();
    let e = inspect_ehci(&mut r, &mut t, &f);
    assert_eq!((e.stopped, e.caplength, e.version, e.ports, e.connected, e.companion_owned), (None, 0x20, 0x0100, 6, 2, 1), "{e:?}");
    assert_eq!((e.usbcmd, e.usbsts, e.configflag, e.halted()), (0x0001_0001, 0, 1, false));
    let mut r2 = ehci_regs();
    r2.set(0x24, 0x0000_1000);
    assert!(inspect_ehci(&mut r2, &mut t, &f).halted(), "HCHalted is USBSTS bit 12");
    r2.log.clear();
    r2.set(0x24, 0x0000_0800);
    assert!(!inspect_ehci(&mut r2, &mut t, &f).halted());
    assert_eq!(e.portsc[..3], [0x1005, 0x1000, 0x3001]);
    assert_eq!(e.legacy, Some((0x68, 0x0001_0001, 1)));
    assert!(r.log.iter().all(|o| *o < EHCI_WINDOW && o % 4 == 0), "{:?}", r.log);
    let mut out = Capture::default();
    render_ehci(&f, &e, &mut out);
    let text = out.text();
    assert!(text.contains("usb: ehci 00:1a.0 8086:8c2d hci 0x0100 ports 6 companions 2 x 3 cmd 0x00010001 sts 0x00000000 halted 0 configflag 1"), "{text}");
    assert!(text.contains(" p1=00001005 p2=00001000 p3=00003001 p4=00000000"), "{text}");
    assert!(text.contains("connected 2 companion-owned 1 legacy@0x68 bios 1 os 0 smi 0x00000001"), "{text}");
    assert!(text.contains("ehci root port 1 connected hs enabled 1 owner ehci (a hub behind it? devices below it are not visible)"), "{text}");
    assert!(text.contains("ehci root port 3 connected"), "{text}");
    assert!(!text.contains("ehci root port 2"), "an empty port is not listed as connected");
    assert!(text.lines().all(|l| l.len() <= 118), "{text}");
}

#[test]
fn a_bad_or_refusing_ehci_stops_without_reading_outside_its_window() {
    let (mut t, f) = ehci_pci();
    let mut r = Regs::default();
    r.set(0, 0xffff_ffff);
    let e = inspect_ehci(&mut r, &mut t, &f);
    assert_eq!((e.stopped, r.reads), (Some("reads as all ones"), 1));
    for cap in [0u32, 8, 0x0c, 0x22, 0x44, 0xff] {
        let mut r = ehci_regs();
        r.set(0, 0x0100_0000 | cap);
        let e = inspect_ehci(&mut r, &mut t, &f);
        assert_eq!(e.stopped, Some("bad CAPLENGTH"), "{cap:#x}");
        assert!(r.log.iter().all(|o| *o < 0x10), "no operational register is read through a bad CAPLENGTH: {:?}", r.log);
    }
    let full = {
        let mut r = ehci_regs();
        inspect_ehci(&mut r, &mut t, &f);
        r.reads
    };
    for k in 0..full {
        let mut r = ehci_regs();
        r.fail_after = Some(k);
        let e = inspect_ehci(&mut r, &mut t, &f);
        assert_eq!((e.stopped, r.reads), (Some("read refused"), k + 1), "fail_after {k}");
    }
    // fifteen ports at the largest CAPLENGTH still end inside the 256-byte window
    let mut r = ehci_regs();
    r.set(0, 0x0100_0040).set(4, 15);
    let e = inspect_ehci(&mut r, &mut t, &f);
    assert_eq!((e.stopped, e.ports), (None, 15));
    assert!(r.log.iter().all(|o| *o < EHCI_WINDOW), "{:?}", r.log);
}

#[test]
fn the_ehci_legacy_capability_list_is_bounded_and_confined_to_the_extended_config_space() {
    for (eecp, want) in [(0u32, false), (0x20, false), (0x3c, false), (0xfc, false), (0xa0, true)] {
        let (mut t, f) = ehci_pci();
        t.set(0, 0x1a, 0, 0xa0, 1 | 1 << 16);
        let mut r = ehci_regs();
        r.set(8, eecp << 8);
        let mut spy = Spy(&mut t, vec![]);
        let e = inspect_ehci(&mut r, &mut spy, &f);
        assert_eq!(e.legacy.is_some(), want, "{eecp:#x}");
        assert!(spy.1.iter().all(|o| (0x40..=0xfc).contains(o)), "{eecp:#x}: {:?}", spy.1);
    }
    // a list that loops through other capabilities ends after eight reads
    let (mut t, f) = ehci_pci();
    t.set(0, 0x1a, 0, 0x68, 0x0000_6802).set(0, 0x1a, 0, 0x6c, 0);
    let mut spy = Spy(&mut t, vec![]);
    let e = inspect_ehci(&mut ehci_regs(), &mut spy, &f);
    assert_eq!(e.legacy, None);
    assert!(spy.1.len() <= 8, "{:?}", spy.1);
    // legacy support behind another capability is found
    let (mut t, f) = ehci_pci();
    t.set(0, 0x1a, 0, 0x68, 0x0000_7000 | 5).set(0, 0x1a, 0, 0x70, 1 << 24 | 1);
    let e = inspect_ehci(&mut ehci_regs(), &mut t, &f);
    assert_eq!(e.legacy.map(|(o, h, _)| (o, h >> 24 & 1)), Some((0x70, 1)));
}

struct EhciHash(u64, u32);
impl Mmio for EhciHash {
    fn read32(&mut self, off: u32) -> Option<u32> {
        assert!(off % 4 == 0 && off < EHCI_WINDOW, "{off:#x}");
        self.1 += 1;
        let mut r = Rng(self.0 ^ (u64::from(off) + 1).wrapping_mul(0x9e37_79b9_7f4a_7c15) | 1);
        Some(match off {
            0 if r.next() % 4 != 0 => 0x0100_0000 | [0x10, 0x20, 0x30, 0x40][(r.next() % 4) as usize],
            _ => r.word(),
        })
    }
}

#[test]
fn random_ehci_register_files_stay_inside_the_window_and_the_read_budget() {
    let (mut t, f) = ehci_pci();
    for seed in 1..=5000u64 {
        let mut m = EhciHash(seed, 0);
        let e = inspect_ehci(&mut m, &mut Noise(seed), &f);
        assert!(m.1 <= 3 + 3 + 15, "seed {seed}: {} reads", m.1);
        assert_eq!(u32::from(e.reads), m.1);
        assert!(e.connected <= e.ports && e.companion_owned <= e.ports && e.ports <= 15);
        let mut out = Capture::default();
        render_ehci(&f, &e, &mut out);
        render_routing(&f, &Routing { xusb2pr: seed as u32, xusb2prm: !(seed as u32), usb3_pssen: seed as u32 >> 3, usb3prm: u32::MAX }, &mut out);
        assert!(out.text().lines().all(|l| l.len() <= 118), "seed {seed}: {}", out.text());
    }
    let _ = &mut t;
}

#[test]
fn the_new_survey_code_never_writes_and_reads_nothing_beyond_its_documented_registers() {
    let hosted = include_str!("../../kernel/rust/usb/xhci/survey.rs");
    for bad in ["fn write", "unsafe", "write_volatile", "outl", "wrmsr"] {
        assert!(!hosted.contains(bad), "survey.rs mentions {bad}");
    }
    // the routing registers are named once, as reads of the four documented offsets
    for off in ["0xd0", "0xd4", "0xd8", "0xdc"] {
        assert_eq!(hosted.matches(&format!("{off})")).count(), 1, "{off} is read exactly once");
    }
    let rs = include_str!("../../arch/x86_64/rust/xhci_survey.rs");
    assert!(!rs.contains("xusb2pr =") && !rs.contains("XUSB2PR ="), "routing is never assigned");
}
