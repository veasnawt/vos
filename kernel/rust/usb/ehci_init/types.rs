//! Typed vocabulary of the EHCI qualification and handoff model: phases, steps, observations, abstract write events,
//! accepted-transition rules and named refusal/failure reasons. Nothing here touches PCI configuration, MMIO or memory;
//! a write is a value.

use crate::facts::EhciFacts;

/// An enum whose variants all carry a stable, unique, human-readable name.
macro_rules! named {
    ($(#[$m:meta])* $v:vis enum $n:ident { $($(#[$vm:meta])* $var:ident => $s:expr),+ $(,)? }) => {
        $(#[$m])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        $v enum $n { $($(#[$vm])* $var),+ }
        impl $n {
            pub const ALL: &'static [$n] = &[$($n::$var),+];
            pub fn name(self) -> &'static str {
                match self { $($n::$var => $s),+ }
            }
        }
    };
}

named! {
    /// Protocol phase. `Reset`, `Failed` and `Refused` are terminal.
    pub enum Phase {
        Idle => "idle",
        Qualified => "qualified",
        Handoff => "handoff",
        HandedOff => "handed-off",
        MaskingSmi => "masking-smi",
        SmiMasked => "smi-masked",
        Stopping => "stopping-schedules",
        Quiet => "quiet",
        Halting => "halting",
        Halted => "halted",
        Resetting => "resetting",
        Reset => "reset",
        Failed => "failed",
        Refused => "refused",
    }
}

impl Phase {
    pub fn terminal(self) -> bool {
        matches!(self, Self::Reset | Self::Failed | Self::Refused)
    }
}

named! {
    /// A transition the caller may request. Each is only accepted from one predecessor phase.
    pub enum Step {
        Handoff => "handoff",
        MaskSmi => "mask-smi",
        StopSchedules => "stop-schedules",
        Halt => "halt",
        Reset => "reset",
    }
}

named! {
    /// A fact the model can name in a refusal.
    pub enum Field {
        Vendor => "vendor",
        Device => "device",
        ClassCode => "class-code",
        HciVersion => "hci-version",
        Caplength => "caplength",
        Ports => "ports",
        Companions => "companions",
        PortsPerCompanion => "ports-per-companion",
        Legacy => "legacy-capability",
        LegacyBios => "legacy-bios-owned",
        LegacyOs => "legacy-os-owned",
        LegacyCtl => "legacy-control",
        Usbcmd => "usbcmd",
        Usbsts => "usbsts",
        Configflag => "configflag",
    }
}

/// Why an operation was refused, or why the machine entered `Failed`/`Refused`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Reason {
    /// The operation is not allowed from the current phase.
    WrongPhase,
    /// The machine is in a terminal phase.
    Terminal,
    /// A required fact is unknown.
    MissingFact(Field),
    /// A required fact is only inferred, never observed.
    InferredOnly(Field),
    /// Two sources disagree about a required fact.
    ContradictoryFact(Field),
    /// The register needed for this write has not been observed since the last write that touches it.
    StaleObservation(Field),
    NotIntel,
    NotEhciClass,
    HciVersionUnsupported,
    PortsInvalid,
    CompanionsPresent,
    LegacyOffsetInvalid,
    CaplengthInvalid,
    SchedulesStillActive,
    NotHalted,
    BadTicks,
    // failure reasons (terminal)
    FirmwareNoRelease,
    FirmwareRevoked,
    SmiNotDisabled,
    ScheduleStopTimeout,
    HaltTimeout,
    ResetTimeout,
    ResetUnverified,
    HostSystemError,
    ReadFailed,
}

impl Reason {
    pub fn name(self) -> &'static str {
        match self {
            Reason::WrongPhase => "wrong-phase",
            Reason::Terminal => "terminal",
            Reason::MissingFact(f) => match f {
                Field::Vendor => "missing.vendor",
                Field::Device => "missing.device",
                Field::ClassCode => "missing.class-code",
                Field::HciVersion => "missing.hci-version",
                Field::Caplength => "missing.caplength",
                Field::Ports => "missing.ports",
                Field::Companions => "missing.companions",
                Field::PortsPerCompanion => "missing.ports-per-companion",
                Field::Legacy => "missing.legacy-capability",
                Field::LegacyBios => "missing.legacy-bios-owned",
                Field::LegacyOs => "missing.legacy-os-owned",
                Field::LegacyCtl => "missing.legacy-control",
                Field::Usbcmd => "missing.usbcmd",
                Field::Usbsts => "missing.usbsts",
                Field::Configflag => "missing.configflag",
            },
            Reason::InferredOnly(f) => match f {
                Field::Vendor => "inferred.vendor",
                Field::Device => "inferred.device",
                Field::ClassCode => "inferred.class-code",
                Field::HciVersion => "inferred.hci-version",
                Field::Caplength => "inferred.caplength",
                Field::Ports => "inferred.ports",
                Field::Companions => "inferred.companions",
                Field::PortsPerCompanion => "inferred.ports-per-companion",
                Field::Legacy => "inferred.legacy-capability",
                Field::LegacyBios => "inferred.legacy-bios-owned",
                Field::LegacyOs => "inferred.legacy-os-owned",
                Field::LegacyCtl => "inferred.legacy-control",
                Field::Usbcmd => "inferred.usbcmd",
                Field::Usbsts => "inferred.usbsts",
                Field::Configflag => "inferred.configflag",
            },
            Reason::ContradictoryFact(f) => match f {
                Field::Vendor => "contradictory.vendor",
                Field::Device => "contradictory.device",
                Field::ClassCode => "contradictory.class-code",
                Field::HciVersion => "contradictory.hci-version",
                Field::Caplength => "contradictory.caplength",
                Field::Ports => "contradictory.ports",
                Field::Companions => "contradictory.companions",
                Field::PortsPerCompanion => "contradictory.ports-per-companion",
                Field::Legacy => "contradictory.legacy-capability",
                Field::LegacyBios => "contradictory.legacy-bios-owned",
                Field::LegacyOs => "contradictory.legacy-os-owned",
                Field::LegacyCtl => "contradictory.legacy-control",
                Field::Usbcmd => "contradictory.usbcmd",
                Field::Usbsts => "contradictory.usbsts",
                Field::Configflag => "contradictory.configflag",
            },
            Reason::StaleObservation(f) => match f {
                Field::LegacyBios | Field::LegacyOs => "stale.legacy-owner",
                Field::LegacyCtl => "stale.legacy-control",
                Field::Usbcmd => "stale.usbcmd",
                Field::Usbsts => "stale.usbsts",
                _ => "stale.other",
            },
            Reason::NotIntel => "not-intel",
            Reason::NotEhciClass => "not-ehci-class",
            Reason::HciVersionUnsupported => "hci-version-unsupported",
            Reason::PortsInvalid => "ports-invalid",
            Reason::CompanionsPresent => "companions-present",
            Reason::LegacyOffsetInvalid => "legacy-offset-invalid",
            Reason::CaplengthInvalid => "caplength-invalid",
            Reason::SchedulesStillActive => "schedules-still-active",
            Reason::NotHalted => "not-halted",
            Reason::BadTicks => "bad-ticks",
            Reason::FirmwareNoRelease => "firmware-no-release",
            Reason::FirmwareRevoked => "firmware-revoked",
            Reason::SmiNotDisabled => "smi-not-disabled",
            Reason::ScheduleStopTimeout => "schedule-stop-timeout",
            Reason::HaltTimeout => "halt-timeout",
            Reason::ResetTimeout => "reset-timeout",
            Reason::ResetUnverified => "reset-unverified",
            Reason::HostSystemError => "host-system-error",
            Reason::ReadFailed => "read-failed",
        }
    }
}

named! {
    /// Why an operation was accepted. Every accepted transition names exactly one rule.
    pub enum Rule {
        Qualified => "qualify.accepted",
        Recorded => "observe.recorded",
        Contradicted => "observe.contradiction",
        HandoffNotNeeded => "handoff.not-needed",
        HandoffAlreadyOwned => "handoff.already-owned",
        HandoffRequest => "handoff.request",
        HandoffWait => "handoff.wait",
        HandoffPending => "handoff.pending",
        HandoffComplete => "handoff.complete",
        HandoffStable => "handoff.stable",
        SmiNotNeeded => "smi.not-needed",
        SmiAlreadyMasked => "smi.already-masked",
        SmiRequest => "smi.request",
        SmiPending => "smi.pending",
        SmiMasked => "smi.masked",
        SchedulesAlreadyOff => "schedules.already-off",
        SchedulesRequest => "schedules.request",
        SchedulesWait => "schedules.wait",
        SchedulesPending => "schedules.pending",
        SchedulesStopped => "schedules.stopped",
        HaltAlreadyHalted => "halt.already-halted",
        HaltRequest => "halt.request",
        HaltWait => "halt.wait",
        HaltPending => "halt.pending",
        HaltObserved => "halt.observed",
        ResetRequest => "reset.request",
        ResetPending => "reset.pending",
        ResetVerified => "reset.verified",
        TickAdvance => "tick.advance",
        FaultTimeout => "fault.timeout",
        FaultRevoked => "fault.firmware-revoked",
        FaultHostError => "fault.host-system-error",
        FaultRead => "fault.read-failed",
        FaultResetUnverified => "fault.reset-unverified",
    }
}

/// A live observation of one register or capability, as a backend would report it. The model never reads anything
/// itself: tests and a future backend supply these values.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Obs {
    Caplength(u8),
    /// USBLEGSUP owner semaphores.
    LegacySup { bios: bool, os: bool },
    LegacyCtl(u32),
    Usbcmd(u32),
    Usbsts(u32),
    Configflag(bool),
    /// The backend could not read what was asked.
    ReadFailed,
}

/// One operation the machine consumes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Operation {
    /// Present the immutable facts. Only valid from `Idle`.
    Qualify(EhciFacts),
    Observe(Obs),
    /// Let `n` abstract time units pass (n >= 1). Deadlines are checked here.
    Advance(u64),
    Request(Step),
}

/// An abstract future hardware write. It is only a description: this crate has no way to perform it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Event {
    /// Byte write of 1 to the OS-owned semaphore (USBLEGSUP bits 31:24, configuration offset `cap + 3`). A byte write
    /// leaves the BIOS-owned byte untouched.
    LegacyOsSemaphore { cap: u8 },
    /// Dword write to USBLEGCTLSTS (configuration offset `cap + 4`) with every SMI enable clear and every status bit 0
    /// (the status bits are write-one-to-clear, so writing 0 leaves them alone).
    LegacyCtlWrite { cap: u8, value: u32 },
    /// Dword write to USBCMD (operational base + 0).
    UsbcmdWrite { value: u32 },
}

/// A transition accepted by `Machine::apply`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Accepted {
    pub from: Phase,
    pub to: Phase,
    pub why: Rule,
    /// The abstract write a backend would perform, if the transition calls for one.
    pub event: Option<Event>,
    /// The failure reason when `to` is `Failed`.
    pub failure: Option<Reason>,
}

/// Whether a step could be planned from the current knowledge of the facts.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Readiness {
    /// Every required fact is observed and consistent.
    Ready,
    /// The step does not apply to this controller (no legacy support capability).
    NotNeeded,
    Blocked(Reason),
}

/// The result of qualification: the controller matches the modelled Intel EHCI shape.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Qualification {
    pub ports: u8,
}
