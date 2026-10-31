//! Fixtures for the two EHCI controllers of the Dell OptiPlex 9020, taken from the physical USB survey v2.1.
//!
//! Source: one owner photograph of the survey screen (the photograph itself is not in the repository). The values are a
//! transcription of that photograph, not an independent register measurement, and every field carries a provenance note.
//! Nothing here names the keyboard or the mouse: the survey shows that each EHCI has one connected, enabled high-speed
//! root port, and does not show what is behind it.

use crate::facts::{EhciFacts, Fact};

/// Source commit of the survey image that produced the photograph.
pub const SOURCE_COMMIT: &str = "602a1dc7f784e4017d9dc2f0624889f700a9c5a2";
/// SHA-256 of the owner's original photograph.
pub const PHOTO_SHA256: &str = "4756f838b29ae1d77e2954d65a0080f0f478f03d0e5d6741532124f61bd78040";
/// SHA-256 of the dual BIOS/UEFI survey image that was booted.
pub const IMAGE_SHA256: &str = "04dad6e50a5c59cc661ddcb71c070a12d26f2c7ce6f808ad7b0b4ee86236195e";

/// How well a fixture field is established.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Note {
    /// Printed by the survey and legible; cross-checked against another printed line.
    Observed,
    /// Printed by the survey; read from the photograph once and not cross-checked.
    ReadOnce,
    /// A conclusion, not a reading.
    Inferred,
    /// The survey did not print it. The fixture leaves the fact `Unknown`.
    Unobserved,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Provenance {
    pub field: &'static str,
    pub note: Note,
    pub survey_line: &'static str,
}

#[derive(Clone, Copy, Debug)]
pub struct Fixture {
    pub name: &'static str,
    /// PCI bus, device, function.
    pub location: (u8, u8, u8),
    pub facts: EhciFacts,
    pub provenance: &'static [Provenance],
}

const fn p(field: &'static str, note: Note, survey_line: &'static str) -> Provenance {
    Provenance { field, note, survey_line }
}

macro_rules! prov {
    ($line:expr, $cmd:expr, $sts:expr, $ctl:expr, $ctl_note:expr) => {
        &[
            p("vendor/device/class", Note::Observed, "usb: ehci ... 0x8086:0x.... cls 0x0c0320"),
            p("hci-version", Note::Observed, $line),
            p("ports", Note::Observed, $line),
            p("companions", Note::Observed, $line),
            p("caplength", Note::Unobserved, "not printed by the survey"),
            p("legacy-capability", Note::Observed, "legacy@0x68 bios 1 os 0"),
            p("legacy-owner-bits", Note::Observed, "legacy@0x68 bios 1 os 0"),
            p("legacy-control", $ctl_note, $ctl),
            p("usbcmd", Note::Observed, $cmd),
            p("usbsts", Note::Observed, $sts),
            p("configflag", Note::Observed, "configflag 1"),
            p("firmware-active", Note::Inferred, "usbcmd periodic-schedule-enable with usbsts periodic-status set"),
            p("root-port-1-device-identity", Note::Unobserved, "connected hs enabled 1 owner ehci (a hub behind it? devices below it are not visible)"),
        ]
    };
}

fn dell_facts(device: u16, usbsts: u32, legacy_ctl: u32) -> EhciFacts {
    EhciFacts {
        vendor: Fact::Observed(0x8086),
        device: Fact::Observed(device),
        class_code: Fact::Observed(0x000c_0320),
        hci_version: Fact::Observed(0x0100),
        // CAPLENGTH, HCSPARAMS and HCCPARAMS were not printed.
        caplength: Fact::Unknown,
        n_ports: Fact::Observed(2),
        n_companions: Fact::Observed(0),
        ports_per_companion: Fact::Observed(0),
        legacy: Fact::Observed(Some(0x68)),
        legacy_bios_owned: Fact::Observed(true),
        legacy_os_owned: Fact::Observed(false),
        legacy_ctl: Fact::Observed(legacy_ctl),
        usbcmd: Fact::Observed(0x0000_0011),
        usbsts: Fact::Observed(usbsts),
        configflag: Fact::Observed(true),
        firmware_active: Fact::Inferred(true),
    }
}

/// Intel EHCI 8086:8c2d at 00:1a.0.
pub fn ehci_8c2d() -> Fixture {
    const PROV: &[Provenance] = prov!(
        "usb: ehci 00:1a.0 8086:8c2d hci 0x0100 ports 2 companions 0 x 0 cmd 0x00000011 sts 0x0000600a halted 0 configflag 1",
        "cmd 0x00000011",
        "sts 0x0000600a",
        "smi 0x000a2005",
        Note::ReadOnce
    );
    Fixture { name: "intel-ehci-8c2d", location: (0x00, 0x1a, 0), facts: dell_facts(0x8c2d, 0x0000_600a, 0x000a_2005), provenance: PROV }
}

/// Intel EHCI 8086:8c26 at 00:1d.0.
pub fn ehci_8c26() -> Fixture {
    const PROV: &[Provenance] = prov!(
        "usb: ehci 00:1d.0 8086:8c26 hci 0x0100 ports 2 companions 0 x 0 cmd 0x00000011 sts 0x00006008 halted 0 configflag 1",
        "cmd 0x00000011",
        "sts 0x00006008",
        "smi 0x00002005",
        Note::ReadOnce
    );
    Fixture { name: "intel-ehci-8c26", location: (0x00, 0x1d, 0), facts: dell_facts(0x8c26, 0x0000_6008, 0x0000_2005), provenance: PROV }
}

/// Both surveyed controllers, in survey order.
pub fn both() -> [Fixture; 2] {
    [ehci_8c2d(), ehci_8c26()]
}
