#![forbid(unsafe_code)]
use core::mem::size_of;
use vos_xhci::xhci::capabilities::Capabilities;
use vos_xhci::xhci::controller::{
    Action, Backend, Controller, Deadline, Observation, PermanentSnapshot, State, Timeouts,
};
use vos_xhci::xhci::resources::{KINDS, Kind, Lease, Usage};
use vos_xhci::xhci::ring::{CommandRing, Completion, Event, EventRing, Region, Trb};
use vos_xhci::xhci::{BAR_BYTES, Error, PAGE, TRBS};
pub mod xhci_fake;
use xhci_fake::{Fake, Trace, capabilities, put};

fn timeouts() -> Timeouts {
    Timeouts {
        ownership: 10,
        halt: 10,
        reset: 10,
        run: 10,
        command: 10,
        stop: 10,
        join: 10,
    }
}
fn qualified() -> Controller {
    let (bytes, facts) = capabilities();
    let mut c = Controller::new(timeouts()).unwrap();
    c.qualify(&bytes, facts).unwrap();
    c
}
fn ready() -> (Controller, Fake) {
    let mut c = qualified();
    let mut b = Fake::new(1);
    c.start(&mut b, 1, 0).unwrap();
    for now in 1..=4 {
        c.step(&mut b, now).unwrap();
    }
    assert_eq!(c.state(), State::Ready);
    (c, b)
}
fn terminal(c: &mut Controller, b: &mut Fake, first: u64) -> State {
    for n in 0..40 {
        if matches!(
            c.state(),
            State::Stopped | State::Failed | State::Quarantined
        ) {
            return c.state();
        }
        b.clear_trace();
        let _ = c.step(b, first + n);
    }
    panic!("model did not reach a bounded terminal state");
}
fn completion(ring: &EventRing, address: u64) -> Completion {
    let mut trb = Fake::completion(address, 1).trb;
    trb.control |= u32::from(ring.cursor().1);
    match ring.inspect(trb, 8).unwrap().unwrap() {
        Event::Completion(c) => c,
        _ => unreachable!(),
    }
}

#[test]
fn valid_capabilities_and_optional_legacy() {
    let (mut bytes, facts) = capabilities();
    let c = Capabilities::parse(&bytes, facts).unwrap();
    assert_eq!(
        (c.slots, c.ports, c.interrupters, c.ac64),
        (64, 8, 16, true)
    );
    assert_eq!(c.legacy, None);
    bytes.copy_within(0x20..0x30, 0x28);
    put(&mut bytes, 0x20, 1 | (2 << 8));
    assert_eq!(
        Capabilities::parse(&bytes, facts).unwrap().legacy,
        Some(0x20)
    );
}
#[test]
fn every_truncated_bar_rejected() {
    let (bytes, facts) = capabilities();
    for n in 0..BAR_BYTES as usize {
        assert_eq!(
            Capabilities::parse(&bytes[..n], facts),
            Err(Error::Truncated)
        );
    }
}
#[test]
fn unsupported_platform_facts_rejected() {
    let (bytes, f) = capabilities();
    for n in 0..14 {
        let mut f = f;
        match n {
            0 => f.bdf = 0x100,
            1 => f.vendor = 0,
            2 => f.device = 0,
            3 => f.candidates = 2,
            4 => f.complete_scan = false,
            5 => f.memory_enabled = false,
            6 => f.d0 = false,
            7 => f.identity_dma = false,
            8 => f.coherent_dma = false,
            9 => f.qualified_resource = false,
            10 => f.ram_disjoint = false,
            11 => f.bar_bytes = 8192,
            12 => f.physical_bits = 53,
            _ => f.bar_address_bits = 16,
        }
        assert_eq!(Capabilities::parse(&bytes, f), Err(Error::Unsupported));
    }
}
#[test]
fn bar_overflow_alignment_and_width() {
    let (bytes, mut f) = capabilities();
    f.bar_base = u64::MAX & !4095;
    assert_eq!(Capabilities::parse(&bytes, f), Err(Error::Overflow));
    f.bar_base = 0xfebf0001;
    assert_eq!(Capabilities::parse(&bytes, f), Err(Error::Alignment));
    f.bar_base = 1 << 40;
    assert_eq!(Capabilities::parse(&bytes, f), Err(Error::Bounds));
    f.physical_bits = 48;
    f.bar_address_bits = 32;
    f.bar_base = 1 << 32;
    assert_eq!(Capabilities::parse(&bytes, f), Err(Error::Bounds));
}
#[test]
fn malformed_and_unsupported_capability_values() {
    let (original, f) = capabilities();
    for (off, value) in [
        (0, 0x1100040),
        (0, 0x1000044),
        (0, 0x1000140),
        (4, 0),
        (4, 0x11001040),
        (8, 1 << 27),
        (8, 1 << 21),
        (0x48, 2),
        (20, 3),
        (24, 1),
        (20, 0xffff_fffc),
        (20, 0x1000),
        (24, 0x4000),
    ] {
        let mut bytes = original;
        put(&mut bytes, off, value);
        assert!(
            Capabilities::parse(&bytes, f).is_err(),
            "offset {off} value {value:x}"
        );
    }
}
#[test]
fn extended_capability_boundaries_and_duplicate_legacy() {
    let (mut bytes, f) = capabilities();
    put(&mut bytes, 16, (0x3000 / 4) << 16 | 1);
    put(&mut bytes, 0x3000, 1 | (2 << 8));
    put(&mut bytes, 0x3008, 1);
    assert_eq!(
        Capabilities::parse(&bytes, f),
        Err(Error::InvalidCapability)
    );
    put(&mut bytes, 16, ((BAR_BYTES - 4) / 4) << 16 | 1);
    put(&mut bytes, BAR_BYTES as usize - 4, 1);
    assert_eq!(Capabilities::parse(&bytes, f), Err(Error::Bounds));
    put(&mut bytes, 16, (0x3000 / 4) << 16 | 1);
    put(&mut bytes, 0x3000, 0);
    assert_eq!(
        Capabilities::parse(&bytes, f),
        Err(Error::InvalidCapability)
    );
    put(&mut bytes, 0x3000, 1 | (1 << 8));
    assert!(Capabilities::parse(&bytes, f).is_err());
}
#[test]
fn exactly_64_extended_headers_are_bounded() {
    let (mut bytes, f) = capabilities();
    put(&mut bytes, 16, (0x3000 / 4) << 16 | 1);
    put(&mut bytes, 0x3000, 0x0200_0402);
    put(&mut bytes, 0x3004, 0x2042_5355);
    put(&mut bytes, 0x3008, 0x0801);
    for n in 0..63 {
        put(
            &mut bytes,
            0x3010 + n * 4,
            128 | if n == 62 { 0 } else { 1 << 8 },
        );
    }
    assert!(Capabilities::parse(&bytes, f).is_ok());
    put(&mut bytes, 0x3010 + 62 * 4, 128 | (1 << 8));
    put(&mut bytes, 0x3010 + 63 * 4, 128);
    assert_eq!(
        Capabilities::parse(&bytes, f),
        Err(Error::InvalidCapability)
    );
}
#[test]
fn generated_capability_mutations_never_escape_bar() {
    let (original, f) = capabilities();
    let mut seed = 0x564f535553423241u64;
    for _ in 0..12000 {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        let mut bytes = original;
        let i = (seed as usize) % 128;
        bytes[i] ^= (seed >> 32) as u8;
        if let Ok(c) = Capabilities::parse(&bytes, f) {
            assert!(c.ports <= 16 && c.operational >= 32 && c.operational % 8 == 0);
            assert!(c.doorbell + 4 <= BAR_BYTES && c.runtime + 0x40 <= BAR_BYTES);
        }
    }
}
#[test]
fn ring_region_rejects_bad_indices_sizes_and_arithmetic() {
    let r = Region::new(0x100000, PAGE, 1 << 30).unwrap();
    for i in 0..TRBS {
        assert_eq!(r.address(i).unwrap(), 0x100000 + u64::from(i) * 16);
    }
    for i in [256, 257, u16::MAX] {
        assert_eq!(r.address(i), Err(Error::Bounds));
    }
    assert_eq!(
        Region::new(u64::MAX & !4095, PAGE, u64::MAX),
        Err(Error::Overflow)
    );
    assert_eq!(Region::new(0x100001, PAGE, 1 << 30), Err(Error::Alignment));
    assert_eq!(Region::new(0x100000, 8192, 1 << 30), Err(Error::Bounds));
    assert_eq!(Region::new(0x100000, PAGE, 0x100001), Err(Error::Bounds));
    assert!(Region::new(0, PAGE, 1 << 30).is_err());
}
#[test]
fn one_command_credit_and_260_round_trips() {
    let region = Region::new(0x101000, PAGE, 1 << 30).unwrap();
    let mut cmd = CommandRing::new(region, 1).unwrap();
    let mut events = EventRing::new(region);
    for n in 0..260 {
        let s = cmd.prepare_noop().unwrap();
        let cursor = cmd.cursor();
        assert_eq!(cmd.prepare_noop(), Err(Error::RingFull));
        assert_eq!(cmd.cursor(), cursor);
        assert_eq!(s.index, n % 255);
        assert_eq!(s.trb.control & 1, u32::from(n < 255));
        if n == 254 {
            let link = s.link.unwrap();
            assert_eq!(link.control, (6 << 10) | 3);
            assert_eq!(link.parameter, region.base());
        } else {
            assert!(s.link.is_none());
        }
        let c = completion(&events, s.id.address());
        cmd.complete(s.id, 1, c).unwrap();
        events.advance().unwrap();
    }
    assert_eq!(cmd.cursor(), (5, false));
    assert_eq!(events.cursor(), (4, false));
    assert_eq!(events.consumed(), 260);
}
#[test]
fn stale_software_ticket_rejected_after_pointer_reuse() {
    let r = Region::new(0x101000, PAGE, 1 << 30).unwrap();
    let mut cmd = CommandRing::new(r, 1).unwrap();
    let events = EventRing::new(r);
    let first = cmd.prepare_noop().unwrap().id;
    let c = completion(&events, first.address());
    cmd.complete(first, 1, c).unwrap();
    for _ in 1..255 {
        let id = cmd.prepare_noop().unwrap().id;
        cmd.complete(id, 1, completion(&events, id.address()))
            .unwrap();
    }
    let fresh = cmd.prepare_noop().unwrap().id;
    assert_eq!(first.address(), fresh.address());
    assert_ne!(first, fresh);
    assert_eq!(cmd.complete(first, 1, c), Err(Error::Stale));
    assert_eq!(cmd.pending(), Some(fresh));
    cmd.complete(fresh, 1, completion(&events, fresh.address()))
        .unwrap();
}
#[test]
fn unexpected_pointer_epoch_duplicate_and_status_rejected() {
    let r = Region::new(0x101000, PAGE, 1 << 30).unwrap();
    let mut cmd = CommandRing::new(r, 4).unwrap();
    let events = EventRing::new(r);
    let id = cmd.prepare_noop().unwrap().id;
    assert_eq!(
        cmd.complete(id, 3, completion(&events, id.address())),
        Err(Error::Stale)
    );
    for address in [0, id.address() + 1, id.address() + 16, u64::MAX] {
        assert_eq!(
            cmd.complete(id, 4, completion(&events, address)),
            Err(Error::UnexpectedEvent)
        );
        assert_eq!(cmd.pending(), Some(id));
    }
    cmd.complete(id, 4, completion(&events, id.address()))
        .unwrap();
    assert_eq!(
        cmd.complete(id, 4, completion(&events, id.address())),
        Err(Error::UnexpectedEvent)
    );
    let mut bad = Fake::completion(id.address(), 4).trb;
    bad.control |= 1;
    bad.status = 2 << 24;
    assert_eq!(events.inspect(bad, 8), Err(Error::UnexpectedEvent));
    assert!(CommandRing::new(r, 0).is_err());
}
#[test]
fn event_cycle_precedes_decoding_and_no_link_in_event_ring() {
    let r = Region::new(0x102000, PAGE, 1 << 30).unwrap();
    let mut events = EventRing::new(r);
    assert_eq!(
        events.inspect(
            Trb {
                parameter: u64::MAX,
                status: u32::MAX,
                control: 0
            },
            8
        ),
        Ok(None)
    );
    let link = Trb {
        parameter: r.base(),
        status: 1 << 24,
        control: (6 << 10) | 1,
    };
    assert!(events.inspect(link, 8).is_err());
    for _ in 0..256 {
        events.advance().unwrap();
    }
    assert_eq!(events.cursor(), (0, false));
    assert_eq!(events.dequeue_address().unwrap(), r.base());
    assert_eq!(
        events
            .inspect(Fake::completion(r.base(), 1).trb, 8)
            .unwrap(),
        Some(Event::Completion(completion(&events, r.base())))
    );
}
#[test]
fn malformed_port_and_reserved_event_fields_rejected() {
    let r = Region::new(0x102000, PAGE, 1 << 30).unwrap();
    let events = EventRing::new(r);
    for port in 0..=255 {
        let mut s = Fake::port(port, 1);
        s.trb.control |= 1;
        assert_eq!(events.inspect(s.trb, 8).is_ok(), (1..=8).contains(&port));
    }
    let mut s = Fake::completion(r.base(), 1).trb;
    s.control |= 1 | (1 << 24);
    assert!(events.inspect(s, 8).is_err());
    let mut s = Fake::port(1, 1).trb;
    s.control |= 1;
    s.parameter |= 1;
    assert!(events.inspect(s, 8).is_err());
}
#[test]
fn accounting_limits_and_underflow_are_transactional() {
    let full = Usage {
        dma_pages: 4,
        control_pages: 1,
        table_pages: 2,
        mappings: 1,
        workers: 1,
        permanent_bytes: 512,
    };
    assert_eq!(full.physical_bytes(), 28 * 1024);
    full.validate().unwrap();
    for extra in [
        Usage {
            dma_pages: 1,
            ..Usage::default()
        },
        Usage {
            control_pages: 1,
            ..Usage::default()
        },
        Usage {
            table_pages: 1,
            ..Usage::default()
        },
        Usage {
            workers: 1,
            ..Usage::default()
        },
        Usage {
            mappings: 1,
            ..Usage::default()
        },
        Usage {
            permanent_bytes: 1,
            ..Usage::default()
        },
    ] {
        assert_eq!(full.add(extra), Err(Error::Budget));
        assert_eq!(full.physical_bytes(), 28 * 1024);
    }
    assert!(Usage::default().subtract(Kind::Command.charge()).is_err());
    let mut invalid = full;
    invalid.permanent_bytes = u16::MAX;
    assert_eq!(
        invalid.add(Usage {
            permanent_bytes: 1,
            ..Usage::default()
        }),
        Err(Error::Overflow)
    );
}
#[test]
fn generated_budget_vectors() {
    for dma in 0..=8 {
        for control in 0..=2 {
            for tables in 0..=4 {
                for workers in 0..=2 {
                    let u = Usage {
                        dma_pages: dma,
                        control_pages: control,
                        table_pages: tables,
                        workers,
                        mappings: 1,
                        permanent_bytes: 512,
                    };
                    assert_eq!(
                        u.validate().is_ok(),
                        dma <= 4
                            && control <= 1
                            && tables <= 2
                            && workers <= 1
                            && u.physical_bytes() <= 28 * 1024
                    );
                }
            }
        }
    }
}
#[test]
fn lease_constructor_rejects_invalid_identity_and_ranges() {
    assert!(Lease::new(Kind::Command, 0, 0x100000).is_err());
    for base in [0, 0x100001, 1 << 30, u64::MAX & !4095] {
        assert!(Lease::new(Kind::Command, 1, base).is_err());
    }
    assert!(Lease::new(Kind::Worker, 1, 0x100000).is_err());
    assert!(Lease::new(Kind::Command, 1, (1 << 30) - PAGE).is_ok());
}
#[test]
fn deadlines_boundaries_wrap_and_clock_regression() {
    let d = Deadline::new(u64::MAX - 3, 5).unwrap();
    assert_eq!(d.expired(0), Ok(false));
    assert_eq!(d.expired(1), Ok(true));
    assert_eq!(
        Deadline::new(100, 5).unwrap().expired(99),
        Err(Error::Clock)
    );
    assert!(Deadline::new(0, 0).is_err());
    assert!(Deadline::new(0, 1 << 63).is_err());
    let mut t = timeouts();
    t.command = 0;
    assert!(Controller::new(t).is_err());
}
#[test]
fn lifecycle_positive_delay_and_ordered_publication() {
    let (mut c, mut b) = ready();
    assert_eq!(c.usage().physical_bytes(), 28 * 1024);
    assert!(c.device_visible());
    b.delay_polls = 2;
    b.clear_trace();
    let id = c.submit_noop(&mut b, 5).unwrap();
    assert_eq!(
        &b.trace[..4],
        &[
            Trace::Payload(0),
            Trace::Cycle(0),
            Trace::WriteBarrier,
            Trace::Doorbell
        ]
    );
    assert_eq!(c.submit_noop(&mut b, 5), Err(Error::RingFull));
    assert_eq!(c.pending(), Some(id));
    c.step(&mut b, 6).unwrap();
    c.step(&mut b, 7).unwrap();
    assert_eq!(c.state(), State::Polling);
    c.step(&mut b, 8).unwrap();
    assert_eq!(c.state(), State::Ready);
    c.shutdown(&mut b, 9).unwrap();
    assert_eq!(terminal(&mut c, &mut b, 10), State::Stopped);
    assert_eq!(c.usage(), Usage::default());
    assert_eq!(b.held, [false; 7]);
    assert!(b.reaped);
}
#[test]
fn no_control_storage_release_before_worker_reaped() {
    let (mut c, mut b) = ready();
    b.ready[Observation::WorkerReaped as usize] = false;
    c.shutdown(&mut b, 5).unwrap();
    c.step(&mut b, 6).unwrap();
    assert_eq!(c.state(), State::Joining);
    assert_eq!(c.usage().dma_pages, 0);
    assert_eq!(c.usage().control_pages, 1);
    assert_eq!(c.usage().workers, 1);
    c.step(&mut b, 7).unwrap();
    assert!(b.held[Kind::Control as usize]);
    b.ready[Observation::WorkerReaped as usize] = true;
    c.step(&mut b, 8).unwrap();
    assert_eq!(c.state(), State::Stopped);
}
#[test]
fn modeled_260_noops_wrap_both_fake_rings() {
    let (mut c, mut b) = ready();
    for n in 0..260 {
        b.clear_trace();
        c.submit_noop(&mut b, 5 + n * 2).unwrap();
        c.step(&mut b, 6 + n * 2).unwrap();
        assert_eq!(c.state(), State::Ready);
    }
    assert_eq!(b.commands[255].control, (6 << 10) | 3);
    c.shutdown(&mut b, 600).unwrap();
    assert_eq!(terminal(&mut c, &mut b, 601), State::Stopped);
    assert_eq!(b.held, [false; 7]);
}
#[test]
fn fake_event_queue_full_and_bounded_service() {
    let (mut c, mut b) = ready();
    b.drop_completion = true;
    c.submit_noop(&mut b, 5).unwrap();
    for _ in 0..256 {
        b.enqueue(Fake::port(1, 1)).unwrap();
    }
    assert_eq!(b.enqueue(Fake::port(1, 1)), Err(Error::RingFull));
    b.clear_trace();
    c.step(&mut b, 6).unwrap();
    assert_eq!(c.port_events(), 32);
    assert_eq!(b.queued, 224);
    assert_eq!(
        b.trace[..b.trace_len]
            .iter()
            .filter(|x| matches!(x, Trace::EventCycle(_)))
            .count(),
        32
    );
    c.cancel(&mut b, 7).unwrap();
    assert_eq!(terminal(&mut c, &mut b, 8), State::Failed);
}
#[test]
fn command_timeout_and_exact_boundary_no_late_retirement() {
    let (mut c, mut b) = ready();
    b.drop_completion = true;
    c.submit_noop(&mut b, 5).unwrap();
    c.step(&mut b, 14).unwrap();
    assert_eq!(c.state(), State::Polling);
    let id = c.pending().unwrap();
    b.enqueue(Fake::completion(id.address(), 1)).unwrap();
    assert_eq!(c.step(&mut b, 15), Err(Error::Timeout));
    assert_eq!(c.pending(), Some(id));
    assert_eq!(terminal(&mut c, &mut b, 16), State::Failed);
    assert_eq!(c.failure(), Some(Error::Timeout));
    assert_eq!(b.held, [false; 7]);
}
#[test]
fn stale_and_unexpected_backend_events_fail_with_cleanup() {
    for mode in 0..3 {
        let (mut c, mut b) = ready();
        b.drop_completion = true;
        let id = c.submit_noop(&mut b, 5).unwrap();
        let mut event = Fake::completion(id.address(), 1);
        match mode {
            0 => event.epoch = 2,
            1 => event.trb.parameter += 16,
            _ => event.trb.control = 6 << 10,
        }
        b.enqueue(event).unwrap();
        assert!(c.step(&mut b, 6).is_err());
        assert_eq!(terminal(&mut c, &mut b, 7), State::Failed);
        assert_eq!(b.held, [false; 7]);
    }
}
#[test]
fn failed_allocation_at_every_stage_rolls_back() {
    for fail in 1..=7 {
        let mut c = qualified();
        let mut b = Fake::new(1);
        b.fail_acquire = Some(fail);
        let result = c.start(&mut b, 1, 0);
        if fail <= 3 {
            assert_eq!(result, Err(Error::Allocation));
        } else {
            result.unwrap();
        }
        assert_eq!(
            terminal(&mut c, &mut b, 1),
            State::Failed,
            "allocation {fail}"
        );
        assert_eq!(c.failure(), Some(Error::Allocation));
        assert_eq!(b.held, [false; 7]);
        assert_eq!(c.usage(), Usage::default());
    }
}
#[test]
fn backend_action_failures_propagate_and_preserve_ownership() {
    for fail in 1..=7 {
        let mut c = qualified();
        let mut b = Fake::new(1);
        b.fail_action = Some(fail);
        let _ = c.start(&mut b, 1, 0);
        assert_eq!(terminal(&mut c, &mut b, 1), State::Failed, "action {fail}");
        assert_eq!(c.failure(), Some(Error::Backend));
        assert_eq!(b.held, [false; 7]);
    }
}
#[test]
fn unconfirmed_quiescence_quarantines_dma_and_mapping_only() {
    let (mut c, mut b) = ready();
    b.ready[Observation::Quiescent as usize] = false;
    c.shutdown(&mut b, 5).unwrap();
    assert_eq!(terminal(&mut c, &mut b, 6), State::Quarantined);
    assert_eq!(c.usage().dma_pages, 4);
    assert_eq!(c.usage().table_pages, 2);
    assert_eq!(c.usage().physical_bytes(), 24 * 1024);
    assert_eq!(c.usage().control_pages, 0);
    assert_eq!(c.usage().workers, 0);
    assert!(c.device_visible());
    let calls = b.action_calls;
    assert_eq!(c.shutdown(&mut b, 30), Err(Error::Quarantined));
    assert_eq!(b.action_calls, calls);
    assert_eq!(c.start(&mut b, 2, 31), Err(Error::IllegalState));
    assert!(c.snapshot().tokens.iter().filter(|x| **x != 0).count() == 5);
}
#[test]
fn join_timeout_retains_control_and_worker() {
    let (mut c, mut b) = ready();
    b.ready[Observation::WorkerReaped as usize] = false;
    c.shutdown(&mut b, 5).unwrap();
    assert_eq!(terminal(&mut c, &mut b, 6), State::Quarantined);
    assert_eq!(c.usage().dma_pages, 0);
    assert_eq!(c.usage().control_pages, 1);
    assert_eq!(c.usage().workers, 1);
    assert!(!b.reaped);
}
#[test]
fn release_failure_is_not_silently_forgotten() {
    let (mut c, mut b) = ready();
    b.fail_release = Some(Kind::Erst);
    c.shutdown(&mut b, 5).unwrap();
    assert_eq!(terminal(&mut c, &mut b, 6), State::Quarantined);
    assert_eq!(c.cleanup_error(), Some(Error::Backend));
    assert!(b.held[Kind::Erst as usize]);
    assert_ne!(c.snapshot().tokens[Kind::Erst as usize], 0);
}
#[test]
fn cancellation_in_every_active_state_and_idempotent_shutdown() {
    for steps in 0..=5 {
        let mut c = qualified();
        let mut b = Fake::new(1);
        c.start(&mut b, 1, 0).unwrap();
        for n in 0..steps.min(4) {
            c.step(&mut b, n + 1).unwrap();
        }
        if steps == 5 {
            b.drop_completion = true;
            c.submit_noop(&mut b, 5).unwrap();
        }
        c.cancel(&mut b, 6).unwrap();
        c.shutdown(&mut b, 6).unwrap();
        assert_eq!(terminal(&mut c, &mut b, 7), State::Failed);
        assert_eq!(c.failure(), Some(Error::Cancelled));
        let calls = b.action_calls;
        for _ in 0..4 {
            c.shutdown(&mut b, 20).unwrap();
        }
        assert_eq!(calls, b.action_calls);
        assert_eq!(b.held, [false; 7]);
    }
}
#[test]
fn ownership_and_other_observation_timeouts_are_bounded() {
    for obs in [
        Observation::Ownership,
        Observation::Halted,
        Observation::ResetReady,
        Observation::Running,
    ] {
        let mut c = qualified();
        let mut b = Fake::new(1);
        b.ready[obs as usize] = false;
        c.start(&mut b, 1, 0).unwrap();
        assert_eq!(terminal(&mut c, &mut b, 1), State::Failed);
        assert_eq!(c.failure(), Some(Error::Timeout));
        assert_eq!(b.held, [false; 7]);
    }
}
#[test]
fn observation_and_submission_error_propagation() {
    for obs in [
        Observation::Ownership,
        Observation::Halted,
        Observation::ResetReady,
        Observation::Running,
    ] {
        let mut c = qualified();
        let mut b = Fake::new(1);
        b.fail_observe = Some(obs);
        c.start(&mut b, 1, 0).unwrap();
        assert_eq!(terminal(&mut c, &mut b, 1), State::Failed);
        assert_eq!(c.failure(), Some(Error::Backend));
        assert_eq!(b.held, [false; 7]);
    }
    let (mut c, mut b) = ready();
    b.fail_publish = true;
    assert_eq!(c.submit_noop(&mut b, 5), Err(Error::Backend));
    assert_eq!(terminal(&mut c, &mut b, 6), State::Failed);
    assert_eq!(b.held, [false; 7]);
}
#[test]
fn no_effects_after_termination_or_illegal_transition() {
    let mut c = qualified();
    let mut b = Fake::new(1);
    assert_eq!(c.submit_noop(&mut b, 0), Err(Error::IllegalState));
    assert_eq!(c.step(&mut b, 0), Err(Error::IllegalState));
    assert_eq!(b.trace_len, 0);
    c.start(&mut b, 1, 0).unwrap();
    for now in 1..=4 {
        c.step(&mut b, now).unwrap();
    }
    c.shutdown(&mut b, 5).unwrap();
    terminal(&mut c, &mut b, 6);
    let trace = b.trace_len;
    for _ in 0..4 {
        assert_eq!(c.submit_noop(&mut b, 30), Err(Error::IllegalState));
        c.step(&mut b, 30).unwrap();
        c.shutdown(&mut b, 30).unwrap();
    }
    assert_eq!(b.trace_len, trace);
}
#[test]
fn repeated_clean_initialization_requires_fresh_epoch_and_no_leaks() {
    let (bytes, facts) = capabilities();
    let mut c = Controller::new(timeouts()).unwrap();
    let mut b = Fake::new(1);
    for epoch in 1..=32 {
        b.clear_trace();
        b.epoch = epoch;
        c.qualify(&bytes, facts).unwrap();
        if epoch > 1 {
            assert_eq!(c.start(&mut b, epoch - 1, 0), Err(Error::Stale));
        }
        c.start(&mut b, epoch, 0).unwrap();
        for now in 1..=4 {
            c.step(&mut b, now).unwrap();
        }
        c.submit_noop(&mut b, 5).unwrap();
        c.step(&mut b, 6).unwrap();
        c.shutdown(&mut b, 7).unwrap();
        assert_eq!(terminal(&mut c, &mut b, 8), State::Stopped);
        assert_eq!(c.usage(), Usage::default());
        assert_eq!(b.held, [false; 7]);
    }
}
#[test]
fn layout_sizes_and_no_global_allocation_requirement() {
    assert!(core::mem::size_of::<Controller>() <= 2048);
    assert!(core::mem::size_of::<PermanentSnapshot>() <= 512);
    assert_eq!(core::mem::size_of::<Trb>(), 16);
    println!(
        "state bytes: Controller={} PermanentSnapshot={} Capabilities={} CommandRing={} EventRing={} Usage={} Fake(host-only)={}",
        size_of::<Controller>(),
        size_of::<PermanentSnapshot>(),
        size_of::<Capabilities>(),
        size_of::<CommandRing>(),
        size_of::<EventRing>(),
        size_of::<Usage>(),
        size_of::<Fake>()
    );
    assert_eq!(KINDS.len(), 7);
    assert!(Kind::Command.dma());
}

#[test]
fn fake_rejects_operations_without_modeled_ownership() {
    let mut b = Fake::new(1);
    assert_eq!(b.action(Action::Halt), Err(Error::Backend));
    assert_eq!(b.action(Action::Reset), Err(Error::Backend));
    assert_eq!(b.observe(Observation::ResetReady), Err(Error::Backend));
}

#[test]
fn dma_width_is_independent_of_bar_width() {
    let (mut bytes, mut facts) = capabilities();
    put(&mut bytes, 16, 0x87000);
    assert!(!Capabilities::parse(&bytes, facts).unwrap().ac64);
    facts.bar_address_bits = 32;
    assert!(Capabilities::parse(&bytes, facts).is_ok());
}
#[test]
fn lease_intervals_reject_partial_overlap_not_just_equal_bases() {
    let mapping = Lease::new(Kind::Mapping, 1, 0x100000).unwrap();
    let inside = Lease::new(Kind::Command, 2, 0x103000).unwrap();
    let outside = Lease::new(Kind::Command, 3, 0x104000).unwrap();
    assert!(mapping.overlaps(&inside));
    assert!(inside.overlaps(&mapping));
    assert!(!mapping.overlaps(&outside));
    assert!(Lease::new(Kind::Mapping, 4, u64::MAX & !4095).is_err());
}
#[test]
fn protocol_capability_body_and_port_range_are_checked() {
    let (mut bytes, facts) = capabilities();
    put(&mut bytes, 0x28, 0);
    assert!(Capabilities::parse(&bytes, facts).is_err());
    put(&mut bytes, 0x28, 0x0802);
    assert!(Capabilities::parse(&bytes, facts).is_err());
    put(&mut bytes, 16, ((BAR_BYTES - 12) / 4) << 16 | 1);
    put(&mut bytes, BAR_BYTES as usize - 12, 2);
    put(&mut bytes, BAR_BYTES as usize - 4, 0x0101);
    assert!(Capabilities::parse(&bytes, facts).is_err());
}
#[test]
fn observation_errors_during_stop_or_join_preserve_remaining_leases() {
    for obs in [Observation::Quiescent, Observation::WorkerReaped] {
        let (mut c, mut b) = ready();
        b.fail_observe = Some(obs);
        c.shutdown(&mut b, 5).unwrap();
        assert_eq!(terminal(&mut c, &mut b, 6), State::Quarantined);
        if obs == Observation::Quiescent {
            assert_eq!(c.usage().dma_pages, 4);
        } else {
            assert_eq!(c.usage().control_pages, 1);
            assert_eq!(c.usage().workers, 1);
        }
    }
}
#[test]
fn stop_and_worker_finish_failures_do_not_release_unknown_ownership() {
    for offset in [1, 2] {
        let (mut c, mut b) = ready();
        b.fail_action = Some(b.action_calls + offset);
        c.shutdown(&mut b, 5).unwrap();
        assert_eq!(terminal(&mut c, &mut b, 6), State::Quarantined);
        if offset == 1 {
            assert_eq!(c.usage().dma_pages, 4);
        } else {
            assert_eq!(c.usage().control_pages, 1);
            assert_eq!(c.usage().workers, 1);
        }
    }
}
#[test]
fn dequeue_error_and_clock_regression_enter_cleanup() {
    let (mut c, mut b) = ready();
    b.fail_consume = true;
    c.submit_noop(&mut b, 5).unwrap();
    assert_eq!(c.step(&mut b, 6), Err(Error::Backend));
    assert_eq!(terminal(&mut c, &mut b, 7), State::Failed);
    assert_eq!(b.held, [false; 7]);
    let (mut c, mut b) = ready();
    b.drop_completion = true;
    c.submit_noop(&mut b, 50).unwrap();
    assert_eq!(c.step(&mut b, 49), Err(Error::Clock));
    assert_eq!(terminal(&mut c, &mut b, 50), State::Failed);
    assert_eq!(b.held, [false; 7]);
}
#[test]
fn failed_qualification_and_duplicate_start_have_no_backend_effect() {
    let (mut bytes, facts) = capabilities();
    put(&mut bytes, 0, 0);
    let mut c = Controller::new(timeouts()).unwrap();
    let mut b = Fake::new(1);
    assert!(c.qualify(&bytes, facts).is_err());
    assert_eq!(c.state(), State::Uninitialized);
    assert_eq!(c.start(&mut b, 1, 0), Err(Error::IllegalState));
    assert_eq!(b.trace_len, 0);
    let mut c = qualified();
    c.start(&mut b, 1, 0).unwrap();
    let calls = b.acquire_calls;
    assert_eq!(c.start(&mut b, 2, 1), Err(Error::IllegalState));
    assert_eq!(calls, b.acquire_calls);
    c.cancel(&mut b, 1).unwrap();
    assert_eq!(terminal(&mut c, &mut b, 2), State::Failed);
}

#[test]
fn required_protocol_and_disjoint_port_groups() {
    let (original, facts) = capabilities();
    let mut bytes = original;
    put(&mut bytes, 16, 1);
    assert_eq!(
        Capabilities::parse(&bytes, facts),
        Err(Error::InvalidCapability)
    );
    bytes = original;
    put(&mut bytes, 0x20, 0x0200_0402);
    put(&mut bytes, 0x30, 0x0300_0002);
    put(&mut bytes, 0x34, 0x2042_5355);
    put(&mut bytes, 0x38, 0x0101);
    assert_eq!(
        Capabilities::parse(&bytes, facts),
        Err(Error::InvalidCapability)
    );
    put(&mut bytes, 0x28, 0x0401);
    put(&mut bytes, 0x38, 0x0405);
    assert!(Capabilities::parse(&bytes, facts).is_ok());
    put(&mut bytes, 0x34, 0);
    assert_eq!(Capabilities::parse(&bytes, facts), Err(Error::Unsupported));
}

#[test]
fn foreign_backend_cannot_take_over_an_active_controller() {
    let (mut c, mut b) = ready();
    let mut foreign = Fake::new(2);
    let before = c.usage();
    assert_eq!(c.submit_noop(&mut foreign, 5), Err(Error::Stale));
    assert_eq!(c.step(&mut foreign, 5), Err(Error::Stale));
    assert_eq!(c.cancel(&mut foreign, 5), Err(Error::Stale));
    assert_eq!(c.shutdown(&mut foreign, 5), Err(Error::Stale));
    assert_eq!(foreign.trace_len, 0);
    assert_eq!(c.usage(), before);
    assert_eq!(c.failure(), None);
    c.shutdown(&mut b, 5).unwrap();
    assert_eq!(terminal(&mut c, &mut b, 6), State::Stopped);
}
#[test]
fn argument_storage_precedes_worker_and_no_worker_failure_releases_it() {
    let mut c = qualified();
    let mut b = Fake::new(1);
    b.fail_acquire = Some(2);
    assert_eq!(c.start(&mut b, 1, 0), Err(Error::Allocation));
    assert_eq!(
        &b.trace[..2],
        &[Trace::Acquire(Kind::Control), Trace::Acquire(Kind::Worker)]
    );
    assert_eq!(c.state(), State::Failed);
    assert_eq!(b.held, [false; 7]);
    assert_eq!(c.usage(), Usage::default());
}
