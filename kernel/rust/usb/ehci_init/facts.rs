//! What is known about an EHCI controller, and how well it is known.
//!
//! Every fact is `Observed` (a register or configuration value that was read), `Inferred` (a conclusion drawn from
//! observations, never read directly), `Unknown` (not available) or `Contradictory` (two sources disagree). Only
//! `Observed` satisfies a safety requirement of the state machine. Nothing here reads hardware: a fact is a value.

/// One piece of knowledge about the controller.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Fact<T> {
    Unknown,
    Observed(T),
    Inferred(T),
    Contradictory,
}

impl<T: Copy + PartialEq> Fact<T> {
    /// The value only when it was observed. An inference or a contradiction never answers a safety question.
    pub fn observed(self) -> Option<T> {
        match self {
            Fact::Observed(v) => Some(v),
            _ => None,
        }
    }

    /// Combine two sources for a value that does not change by itself (identity, port counts, capability offset).
    ///
    /// Two observations that differ, or two inferences that differ, are `Contradictory`. An observation outranks an
    /// inference without making the inference a contradiction: the inference is simply replaced. `Contradictory` is
    /// absorbing.
    pub fn reconcile(self, other: Fact<T>) -> Fact<T> {
        match (self, other) {
            (Fact::Contradictory, _) | (_, Fact::Contradictory) => Fact::Contradictory,
            (Fact::Unknown, x) | (x, Fact::Unknown) => x,
            (Fact::Observed(a), Fact::Observed(b)) => {
                if a == b {
                    Fact::Observed(a)
                } else {
                    Fact::Contradictory
                }
            }
            (Fact::Observed(a), Fact::Inferred(_)) | (Fact::Inferred(_), Fact::Observed(a)) => Fact::Observed(a),
            (Fact::Inferred(a), Fact::Inferred(b)) => {
                if a == b {
                    Fact::Inferred(a)
                } else {
                    Fact::Contradictory
                }
            }
        }
    }
}

/// The facts of one EHCI controller. All fields start `Unknown`; a survey fixture sets what was observed.
///
/// Stable fields (identity, `caplength`, port and companion counts, the legacy capability offset) are reconciled with
/// later observations. Register-state fields (`usbcmd`, `usbsts`, the legacy owner bits, `legacy_ctl`, `configflag`) are
/// replaced by later observations, because they change as the protocol runs.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EhciFacts {
    pub vendor: Fact<u16>,
    pub device: Fact<u16>,
    /// PCI class code as base, sub-class and programming interface in one value (EHCI is `0x0c0320`).
    pub class_code: Fact<u32>,
    pub hci_version: Fact<u16>,
    /// CAPLENGTH: where the operational registers start. Needed before any operational register is addressed.
    pub caplength: Fact<u8>,
    pub n_ports: Fact<u8>,
    /// N_CC: number of companion controllers.
    pub n_companions: Fact<u8>,
    /// N_PCC: ports per companion controller.
    pub ports_per_companion: Fact<u8>,
    /// Offset of the USB legacy support capability in PCI configuration space. `Observed(None)` is "looked for, absent".
    pub legacy: Fact<Option<u8>>,
    /// USBLEGSUP BIOS-owned semaphore (bit 16).
    pub legacy_bios_owned: Fact<bool>,
    /// USBLEGSUP OS-owned semaphore (bit 24).
    pub legacy_os_owned: Fact<bool>,
    /// USBLEGCTLSTS.
    pub legacy_ctl: Fact<u32>,
    pub usbcmd: Fact<u32>,
    pub usbsts: Fact<u32>,
    pub configflag: Fact<bool>,
    /// Whether firmware is actively using the controller. Only ever an inference; no transition depends on it.
    pub firmware_active: Fact<bool>,
}

impl EhciFacts {
    pub const UNKNOWN: EhciFacts = EhciFacts {
        vendor: Fact::Unknown,
        device: Fact::Unknown,
        class_code: Fact::Unknown,
        hci_version: Fact::Unknown,
        caplength: Fact::Unknown,
        n_ports: Fact::Unknown,
        n_companions: Fact::Unknown,
        ports_per_companion: Fact::Unknown,
        legacy: Fact::Unknown,
        legacy_bios_owned: Fact::Unknown,
        legacy_os_owned: Fact::Unknown,
        legacy_ctl: Fact::Unknown,
        usbcmd: Fact::Unknown,
        usbsts: Fact::Unknown,
        configflag: Fact::Unknown,
        firmware_active: Fact::Unknown,
    };
}

impl Default for EhciFacts {
    fn default() -> Self {
        Self::UNKNOWN
    }
}
