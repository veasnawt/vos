//! USB.2-C3a dry-run halt/reset lifecycle. It plans and validates a sequence;
//! nothing in VOS executes it. The step vocabulary maps onto the canonical
//! USB.2-A `Action`/`Observation`; run, program, enable-master, doorbells and
//! ownership requests are not expressible, so no plan can contain them.
use super::controller::{Action, Observation};

pub const PLAN_LIMIT: usize = 24;
pub const INTERRUPTERS: u8 = 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    /// Clear IMAN.IE of one interrupter.
    MaskInterrupter(u8),
    Halt,
    AwaitHalted,
    DisableMaster,
    Reset,
    AwaitResetReady,
    /// Read back reset values of every ring pointer and interrupter register.
    VerifyCleared,
}
impl Step {
    pub fn action(self) -> Option<Action> {
        match self {
            Self::Halt => Some(Action::Halt),
            Self::DisableMaster => Some(Action::DisableMaster),
            Self::Reset => Some(Action::Reset),
            _ => None,
        }
    }
    pub fn observation(self) -> Option<Observation> {
        match self {
            Self::AwaitHalted => Some(Observation::Halted),
            Self::AwaitResetReady => Some(Observation::ResetReady),
            _ => None,
        }
    }
    pub fn code(self) -> u8 {
        match self {
            Self::MaskInterrupter(i) => 0x10 | (i & 0xf),
            Self::Halt => 1,
            Self::AwaitHalted => 2,
            Self::DisableMaster => 3,
            Self::Reset => 4,
            Self::AwaitResetReady => 5,
            Self::VerifyCleared => 6,
        }
    }
}

/// Inputs from the qualification and residue assessment.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Facts {
    pub qualified: bool,
    pub residue_complete: bool,
    pub halted: bool,
    pub run_stop: bool,
    pub bus_master: bool,
    pub controller_ready: bool,
    pub interrupter_enables: u16,
    pub legacy_present: bool,
    pub legacy_os_owned: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    Unqualified,
    ResidueIncomplete,
    NotReady,
    Inconsistent,
    LegacyNotOwned,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Violation {
    Refused(Refusal),
    Length,
    Unneeded,
    Duplicate,
    Order,
    MissingMask,
    MissingHalt,
    MissingMasterOff,
    Incomplete,
    Trailing,
}

impl Violation {
    /// Stable report code: refusals 0x10 + refusal, rules 1..=9.
    pub fn code(self) -> u32 {
        match self {
            Self::Refused(r) => 0x10 + r as u32,
            Self::Length => 1,
            Self::Unneeded => 2,
            Self::Duplicate => 3,
            Self::Order => 4,
            Self::MissingMask => 5,
            Self::MissingHalt => 6,
            Self::MissingMasterOff => 7,
            Self::Incomplete => 8,
            Self::Trailing => 9,
        }
    }
}

/// Preconditions shared by planning and validation.
pub fn admit(f: &Facts) -> Result<(), Refusal> {
    if !f.qualified {
        return Err(Refusal::Unqualified);
    }
    if !f.residue_complete {
        return Err(Refusal::ResidueIncomplete);
    }
    if !f.controller_ready {
        return Err(Refusal::NotReady);
    }
    if f.halted == f.run_stop {
        return Err(Refusal::Inconsistent); // halted with R/S set, or running without
    }
    if f.legacy_present && !f.legacy_os_owned {
        return Err(Refusal::LegacyNotOwned); // C3a cannot plan a firmware handoff
    }
    Ok(())
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Plan {
    steps: [Step; PLAN_LIMIT],
    len: u8,
}
impl Plan {
    pub fn steps(&self) -> &[Step] {
        // `get` keeps this panic-free: the kernel links no core panic machinery.
        self.steps.get(..usize::from(self.len)).unwrap_or(&[])
    }
}

/// Minimal sequence: mask enabled interrupters, establish the halt (writing
/// R/S only if it is set), drop bus mastering, reset, await ready, verify.
pub fn plan(f: &Facts) -> Result<Plan, Refusal> {
    admit(f)?;
    let mut p = Plan { steps: [Step::VerifyCleared; PLAN_LIMIT], len: 0 };
    let mut push = |s| {
        if let Some(slot) = p.steps.get_mut(usize::from(p.len)) {
            *slot = s;
            p.len += 1;
        }
    };
    for i in 0..INTERRUPTERS {
        if f.interrupter_enables & (1 << i) != 0 {
            push(Step::MaskInterrupter(i));
        }
    }
    if f.run_stop {
        push(Step::Halt);
    }
    push(Step::AwaitHalted);
    if f.bus_master {
        push(Step::DisableMaster);
    }
    push(Step::Reset);
    push(Step::AwaitResetReady);
    push(Step::VerifyCleared);
    Ok(p)
}

/// Validate any proposed sequence against the ordering rules. Every write
/// must be needed, appear once and follow its preconditions.
pub fn validate(steps: &[Step], f: &Facts) -> Result<(), Violation> {
    admit(f).map_err(Violation::Refused)?;
    if steps.len() > PLAN_LIMIT {
        return Err(Violation::Length);
    }
    let (mut masked, mut halt, mut halted, mut master_off) = (0u16, false, false, !f.bus_master);
    let (mut reset, mut ready, mut verified) = (false, false, false);
    for &s in steps {
        if verified {
            return Err(Violation::Trailing);
        }
        match s {
            Step::MaskInterrupter(i) => {
                let bit = if i < INTERRUPTERS { 1u16 << i } else { 0 };
                if bit == 0 || f.interrupter_enables & bit == 0 {
                    return Err(Violation::Unneeded);
                }
                if masked & bit != 0 {
                    return Err(Violation::Duplicate);
                }
                if halt || halted || reset || master_off != !f.bus_master {
                    return Err(Violation::Order); // masks precede every other step
                }
                masked |= bit;
            }
            Step::Halt => {
                if !f.run_stop {
                    return Err(Violation::Unneeded);
                }
                if halt {
                    return Err(Violation::Duplicate);
                }
                if masked != f.interrupter_enables {
                    return Err(Violation::MissingMask);
                }
                halt = true;
            }
            Step::AwaitHalted => {
                if halted {
                    return Err(Violation::Duplicate);
                }
                if f.run_stop && !halt {
                    return Err(Violation::MissingHalt);
                }
                if masked != f.interrupter_enables {
                    return Err(Violation::MissingMask);
                }
                halted = true;
            }
            Step::DisableMaster => {
                if !f.bus_master {
                    return Err(Violation::Unneeded);
                }
                if master_off {
                    return Err(Violation::Duplicate);
                }
                if !halted {
                    return Err(Violation::MissingHalt);
                }
                master_off = true;
            }
            Step::Reset => {
                if reset {
                    return Err(Violation::Duplicate);
                }
                if !halted {
                    return Err(Violation::MissingHalt);
                }
                if !master_off {
                    return Err(Violation::MissingMasterOff);
                }
                reset = true;
            }
            Step::AwaitResetReady => {
                if ready {
                    return Err(Violation::Duplicate);
                }
                if !reset {
                    return Err(Violation::Order);
                }
                ready = true;
            }
            Step::VerifyCleared => {
                if !ready {
                    return Err(Violation::Order);
                }
                verified = true;
            }
        }
    }
    if verified { Ok(()) } else { Err(Violation::Incomplete) }
}
