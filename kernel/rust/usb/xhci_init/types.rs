//! Typed vocabulary of the C4.1 initialization protocol model: phases, operations,
//! register-write model events, justifications and named refusal/failure reasons.
//! Nothing here touches PCI configuration, MMIO or memory; a write is a value.

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
    /// Protocol phase. `Refused`, `Failed`, `Quarantined` and `Quiesced` are terminal.
    pub enum Phase {
        Idle => "idle",
        Qualified => "qualified",
        Handoff => "handoff",
        HandedOff => "handed-off",
        Owned => "owned",
        Halting => "halting",
        Halted => "halted",
        Resetting => "resetting",
        Contained => "contained",
        Programming => "programming",
        Programmed => "programmed",
        BusMastering => "bus-mastering",
        EventRingArmed => "event-ring-armed",
        Starting => "starting",
        Running => "running",
        Quiesced => "quiesced",
        Failed => "failed",
        Quarantined => "quarantined",
        Refused => "refused",
    }
}

impl Phase {
    pub fn terminal(self) -> bool {
        matches!(self, Self::Quiesced | Self::Failed | Self::Quarantined | Self::Refused)
    }
}

named! {
    /// Why an operation was accepted. Every accepted transition names exactly one rule.
    pub enum Rule {
        QualifyNoLegacy => "qualify.no-legacy",
        QualifyLegacy => "qualify.legacy",
        TickAdvance => "tick.advance",
        HandoffRequest => "handoff.request",
        HandoffPending => "handoff.pending",
        HandoffComplete => "handoff.complete",
        HandoffStable => "handoff.stable",
        SmiDisabled => "smi.disabled",
        HaltRequest => "halt.request",
        HaltIdleObserved => "halt.idle-observed",
        HaltObserved => "halt.observed",
        HaltPending => "halt.pending",
        StatusStable => "status.stable",
        BmeClear => "bme.clear",
        BmeSet => "bme.set",
        PciStable => "pci.stable",
        ResetAssert => "reset.assert",
        ResetPending => "reset.pending",
        ResetVerified => "reset.verified",
        ResetQuiesced => "reset.quiesced",
        ProgramSlots => "program.slots",
        ProgramDcbaap => "program.dcbaap",
        ProgramCrcr => "program.crcr",
        ProgramErstsz => "program.erstsz",
        ProgramErdp => "program.erdp",
        ErstbaArmed => "erstba.armed",
        ArmedStatusClean => "armed.status-clean",
        RunStart => "run.start",
        RunStarted => "run.started",
        RunPending => "run.pending",
        CmdNoopStaged => "cmd.noop-staged",
        CmdLinkPublished => "cmd.link-published",
        DoorbellNoop => "doorbell.noop",
        EventEmpty => "event.empty",
        EventCompletion => "event.completion",
        EventPortChange => "event.port-change",
        AckErdp => "ack.erdp",
        PollStep => "poll.step",
        PortsQuiet => "ports.quiet",
        PortsDeviceNoted => "ports.device-noted",
        CancelHalt => "cancel.halt",
        DmaAllocate => "dma.allocate",
        DmaZero => "dma.zero",
        DmaPrepare => "dma.prepare",
        DmaRelease => "dma.release",
        CleanupRelease => "cleanup.release",
        QuarantineHalt => "quarantine.halt",
        QuarantineHalted => "quarantine.halted",
        QuarantineBmeClear => "quarantine.bme-clear",
        QuarantineVerified => "quarantine.verified",
        QuarantineBmeStuck => "quarantine.bme-stuck",
        FaultStatus => "fault.status-error",
        FaultBackend => "fault.backend",
        FaultTimeout => "fault.timeout",
        FaultOwnership => "fault.ownership",
        FaultBmeMismatch => "fault.bme-mismatch",
        FaultMsi => "fault.msi",
        FaultDevice => "fault.device-attached",
        FaultEvent => "fault.event",
        FaultClock => "fault.clock",
        FaultResetUnverified => "fault.reset-unverified",
        FaultInconsistent => "fault.inconsistent",
        FaultUnexpectedHalt => "fault.unexpected-halt",
        FaultCancelled => "fault.cancelled",
        FaultRefused => "fault.qualification-refused",
    }
}

named! {
    /// A named refusal (operation rejected, state unchanged) or failure cause (terminal).
    pub enum Reason {
        // qualification
        UnqualifiedPlatform => "unqualified-platform",
        UnsupportedVersion => "unsupported-version",
        PageSizeUnsupported => "page-size-unsupported",
        BadCapabilities => "bad-capabilities",
        BadDmaLimit => "bad-dma-limit",
        ControllerFault => "controller-fault",
        InconsistentRunState => "inconsistent-run-state",
        IommuPresent => "iommu-present",
        IommuUnresolved => "iommu-unresolved",
        IdentityDmaDenied => "identity-dma-denied",
        IdentityDmaUnresolved => "identity-dma-unresolved",
        NoSnoopEnabled => "no-snoop-enabled",
        NoSnoopUnresolved => "no-snoop-unresolved",
        MsiEnabled => "msi-enabled",
        DbcEnabled => "dbc-enabled",
        DbcUnresolved => "dbc-unresolved",
        ScratchpadsInvalid => "scratchpads-invalid",
        ScratchpadsExceedBudget => "scratchpads-exceed-budget",
        OwnershipConflict => "ownership-conflict",
        DeviceAttached => "device-attached",
        // sequencing
        NotQualified => "not-qualified",
        AlreadyQualified => "already-qualified",
        AfterTerminal => "after-terminal",
        WrongPhase => "wrong-phase",
        ObservationNotExpected => "observation-not-expected",
        NoLegacyCapability => "no-legacy-capability",
        AlreadyOsOwned => "already-os-owned",
        SmiBeforeOwnership => "smi-before-ownership",
        OwnershipUnresolved => "ownership-unresolved",
        OwnershipLost => "ownership-lost",
        AlreadyHalted => "already-halted",
        HaltNotRequested => "halt-not-requested",
        UseCancel => "use-cancel",
        UseClearRunStop => "use-clear-run-stop",
        BmeClearBeforeHalt => "bme-clear-before-halt",
        BusMasterAlreadyClear => "bus-master-already-clear",
        BusMasterAlreadySet => "bus-master-already-set",
        BusMasterBeforeProgramming => "bus-master-before-programming",
        BusMasterUnverified => "bus-master-unverified",
        BusMasterMismatch => "bus-master-mismatch",
        ResetBeforeHalt => "reset-before-halt",
        ResetWithBusMaster => "reset-with-bus-master",
        ResetUnverified => "reset-unverified",
        ProgramBeforeContainment => "program-before-containment",
        ProgrammingClosed => "programming-closed",
        RegisterRepeated => "register-repeated",
        SlotsInvalid => "slots-invalid",
        ErstSizeInvalid => "erst-size-invalid",
        CycleStateInvalid => "cycle-state-invalid",
        EhbNotExpected => "ehb-not-expected",
        PointerMismatch => "pointer-mismatch",
        CrcrWhileRunning => "crcr-while-running",
        ErstbaBeforeStatic => "erstba-before-static",
        ErstbaWithoutBusMaster => "erstba-without-bus-master",
        RunBeforeEventRing => "run-before-event-ring",
        StatusNotChecked => "status-not-checked",
        AlreadyRunning => "already-running",
        NotRunning => "not-running",
        // command ring
        CommandIndexMismatch => "command-index-mismatch",
        CommandCycleMismatch => "command-cycle-mismatch",
        CommandOutstanding => "command-outstanding",
        NothingStaged => "nothing-staged",
        LinkNotExpected => "link-not-expected",
        LinkToggleMissing => "link-toggle-missing",
        LinkTargetMismatch => "link-target-mismatch",
        LinkMissing => "link-missing",
        // event ring
        EventIndexMismatch => "event-index-mismatch",
        EventCycleMismatch => "event-cycle-mismatch",
        PollBudgetExceeded => "poll-budget-exceeded",
        AckMissing => "ack-missing",
        ErdpMismatch => "erdp-mismatch",
        EhbNotSet => "ehb-not-set",
        ErdpRepeat => "erdp-repeat",
        UnexpectedCompletion => "unexpected-completion",
        CompletionPointerMismatch => "completion-pointer-mismatch",
        CompletionFieldInvalid => "completion-field-invalid",
        CommandFailed => "command-failed",
        PortOutOfRange => "port-out-of-range",
        UnexpectedEventType => "unexpected-event-type",
        UnexpectedHalt => "unexpected-halt",
        PortMaskInvalid => "port-mask-invalid",
        // DMA pages
        PageAlreadyOwned => "page-already-owned",
        PageNotOwned => "page-not-owned",
        PageState => "page-state",
        PageNotPrepared => "page-not-prepared",
        PageStillPublished => "page-still-published",
        PageOverlap => "page-overlap",
        Misaligned => "misaligned",
        AddressRange => "address-range",
        AllocationClosed => "allocation-closed",
        DependencyNotReady => "dependency-not-ready",
        ErstSegmentMismatch => "erst-segment-mismatch",
        ErstSegmentTooSmall => "erst-segment-too-small",
        ErstSegmentSizeUnsupported => "erst-segment-size-unsupported",
        ScratchpadPointerUnsupported => "scratchpad-pointer-unsupported",
        ScratchpadArrayRequired => "scratchpad-array-required",
        BackendFault => "backend-fault",
        ReleaseForbiddenByPolicy => "release-forbidden-by-policy",
        // forbidden writes
        ForbiddenInterruptEnable => "forbidden-interrupt-enable",
        ForbiddenPortWrite => "forbidden-port-write",
        ForbiddenSlotDoorbell => "forbidden-slot-doorbell",
        ForbiddenCommand => "forbidden-command",
        // timeouts and clock
        HandoffTimeout => "handoff-timeout",
        HaltTimeout => "halt-timeout",
        ResetTimeout => "reset-timeout",
        StartTimeout => "start-timeout",
        CommandTimeout => "command-timeout",
        ClockBackwards => "clock-backwards",
        Cancelled => "cancelled",
    }
}

named! {
    /// A tri-state platform fact. `Unknown` is never treated as favourable.
    pub enum Tri {
        No => "no",
        Yes => "yes",
        Unknown => "unknown",
    }
}

named! {
    /// Platform profile. Only the QEMU 10.2.1 xHCI profile is qualified for this model.
    pub enum Profile {
        Qemu1021 => "qemu-10.2.1",
        Unqualified => "unqualified",
    }
}

named! {
    pub enum Dbc {
        Absent => "absent",
        Disabled => "disabled",
        Enabled => "enabled",
        Unknown => "unknown",
    }
}

named! {
    /// DMA page roles (one page each).
    pub enum PageKind {
        Command => "command",
        Event => "event",
        Dcbaa => "dcbaa",
        Erst => "erst",
    }
}

named! {
    pub enum PageState {
        Free => "free",
        Allocated => "allocated",
        Zeroed => "zeroed",
        Prepared => "prepared",
        Published => "published",
    }
}

named! {
    /// What happens to a published page after a verified quiescence.
    pub enum ReleasePolicy {
        AfterQuiesce => "after-quiesce",
        Never => "never",
    }
}

named! {
    /// Writes the model forbids unconditionally (interrupts, ports, slots, other commands).
    pub enum Forbidden {
        InterruptEnable => "interrupt-enable",
        PortWrite => "port-write",
        SlotDoorbell => "slot-doorbell",
        Command => "command",
    }
}

/// USB Legacy Support capability state (xHCI 1.2 section 7.1.1 semaphores).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Legacy {
    Absent,
    Present { bios: bool, os: bool },
}

/// Qualification input: everything the model needs to know before it accepts a controller.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Facts {
    pub profile: Profile,
    /// HCIVERSION as read (0x0100 is xHCI 1.0).
    pub version: u16,
    pub page_4k: bool,
    pub slots: u8,
    pub ports: u8,
    pub interrupters: u16,
    /// Max Scratchpad Buffers (HCSPARAMS2 Hi:Lo).
    pub scratchpads: u16,
    pub legacy: Legacy,
    pub dbc: Dbc,
    /// `Yes` means an IOMMU is present; only an established `No` qualifies.
    pub iommu: Tri,
    /// `Yes` means identity-mapped DMA is established; only `Yes` qualifies.
    pub identity_dma: Tri,
    /// `Yes` means PCIe Enable No Snoop is set; only an established `No` qualifies.
    pub no_snoop: Tri,
    /// MSI or MSI-X enabled (observed, never written by this protocol).
    pub msi: bool,
    pub bme: bool,
    /// USBCMD.R/S as read.
    pub running: bool,
    /// USBSTS.HCH as read.
    pub halted: bool,
    pub hse: bool,
    pub hce: bool,
    /// Ports with a connected device, one bit per port.
    pub connected: u32,
    /// Exclusive upper bound of device-addressable RAM for DMA pages.
    pub dma_limit: u64,
}

/// USBSTS bits relevant to the protocol (xHCI 1.2 Table 5-21).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Status {
    pub hch: bool,
    pub hse: bool,
    pub hce: bool,
}

/// Read-back after HCRST (xHCI 1.2 sections 4.2 and 5.4.1; reset values 4.23.1).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ResetState {
    /// USBSTS.CNR still set: operational and runtime registers must not be written.
    pub cnr: bool,
    pub hch: bool,
    pub usbcmd_zero: bool,
    /// DNCTRL, CONFIG, CRCR, DCBAAP and every interrupter's ring registers at reset values
    /// (the platform decides this per profile; QEMU's CRCR read quirk is its business).
    pub pointers_zero: bool,
    pub bme: bool,
    /// No port reports a connection, enable, reset or over-current (change bits ignored).
    pub ports_quiet: bool,
}

/// An event TRB as decoded by the platform (typed, not raw bytes).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EventTrb {
    /// Ring index the platform read it from.
    pub index: u16,
    pub cycle: bool,
    /// TRB type: 33 Command Completion, 34 Port Status Change.
    pub kind: u8,
    /// Command TRB pointer (completion events).
    pub pointer: u64,
    /// Completion code (1 = Success).
    pub code: u8,
    /// Command completion parameter (must be 0 for No-Op).
    pub param: u32,
    pub slot: u8,
    pub vf: u8,
    /// Port ID (port status change events).
    pub port: u8,
}

/// One typed operation. Writes are *proposals*: accepting one yields a `ModelEvent`
/// and never performs it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Operation {
    // observations and time
    Qualify(Facts),
    Tick(u64),
    ObserveHandoff { bios: bool, os: bool },
    ObserveStatus(Status),
    ObserveReset(ResetState),
    ObservePci { bme: bool, msi: bool },
    ObservePorts { connected: u32 },
    ConsumeEvent(Option<EventTrb>),
    // host-side DMA page lifecycle
    DmaAllocate { kind: PageKind, addr: u64 },
    DmaZero(PageKind),
    PrepareCommandRing,
    PrepareEventRing,
    PrepareDcbaa { entry0: u64 },
    PrepareErst { segment: u64, size: u16 },
    DmaRelease(PageKind),
    // typed device writes (model events)
    RequestOsOwnership,
    DisableLegacySmi,
    ClearRunStop,
    ClearBusMaster,
    AssertReset,
    SetMaxSlots(u8),
    SetDcbaap(u64),
    SetCommandRing { addr: u64, rcs: bool },
    SetErstSize(u16),
    SetEventDequeue { addr: u64, ehb: bool },
    SetBusMaster,
    SetErstBase(u64),
    SetRunStop,
    // command ring staging and doorbell
    PublishNoop { index: u16, cycle: bool },
    PublishLink { index: u16, cycle: bool, toggle: bool, target: u64 },
    RingCommandDoorbell,
    // control
    BeginPollStep,
    Cancel,
    /// The backend that supplies observations failed: terminal path (a session injects this, never a caller).
    BackendFault,
    Forbidden(Forbidden),
}

named! {
    /// Register or configuration target of a model write event.
    pub enum Target {
        LegacyOsOwned => "usblegsup.os-owned",
        LegacySmi => "usblegctlsts.smi-enable",
        RunStop => "usbcmd.run-stop",
        Hcrst => "usbcmd.hcrst",
        PciBme => "pci.command.bme",
        Config => "config",
        Dcbaap => "dcbaap",
        Crcr => "crcr",
        Erstsz => "ir0.erstsz",
        Erdp => "ir0.erdp",
        Erstba => "ir0.erstba",
        Doorbell0 => "doorbell0",
    }
}

/// A register write as a model event. `id` is the Gate 0 inventory entry (N-01 ... N-18).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ModelEvent {
    pub id: &'static str,
    pub target: Target,
    /// Access width in bits.
    pub width: u8,
    pub value: u64,
}

/// An accepted transition: where it went and which rule justified it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Accepted {
    pub from: Phase,
    pub to: Phase,
    pub why: Rule,
    pub event: Option<ModelEvent>,
}
