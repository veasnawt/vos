//! USB.2-C2 pure ownership-model tests over a fake frame provider.
#![forbid(unsafe_code)]
use vos_xhci::xhci::Error;
use vos_xhci::xhci::dma::{
    ACTIVE_LIMIT, FRAME_HIGH, Frames, Page, Pool, QUARANTINE_LIMIT, RECORDS, Reason, State,
};
use vos_xhci::xhci::resources::Kind;

const SET: [Kind; 4] = [Kind::Command, Kind::Event, Kind::Dcbaa, Kind::Erst];

/// Frames handed out in order from `next`; any call can be made to fail.
struct Fake {
    next: Vec<u64>,
    held: Vec<u64>,
    zeroed: Vec<u64>,
    freed: u32,
    fail_alloc: Option<u32>,
    fail_zero: Option<u32>,
    fail_free: Option<u32>,
    calls: [u32; 3],
}
impl Fake {
    fn new() -> Self {
        Fake {
            next: (0..64).map(|i| 0x20_0000 + i * 0x1000).rev().collect(),
            held: vec![],
            zeroed: vec![],
            freed: 0,
            fail_alloc: None,
            fail_zero: None,
            fail_free: None,
            calls: [0; 3],
        }
    }
    fn with(frames: &[u64]) -> Self {
        let mut f = Fake::new();
        f.next = frames.iter().rev().copied().collect();
        f
    }
    fn hit(&mut self, op: usize, at: Option<u32>) -> bool {
        let n = self.calls[op];
        self.calls[op] += 1;
        at == Some(n)
    }
}
impl Frames for Fake {
    fn alloc(&mut self) -> Result<u64, Error> {
        if self.hit(0, self.fail_alloc) {
            return Err(Error::Allocation);
        }
        let frame = self.next.pop().ok_or(Error::Allocation)?;
        self.held.push(frame);
        Ok(frame)
    }
    fn zero(&mut self, frame: u64) -> Result<(), Error> {
        assert!(self.held.contains(&frame), "zero of a frame the pool does not own");
        if self.hit(1, self.fail_zero) {
            return Err(Error::Backend);
        }
        self.zeroed.push(frame);
        Ok(())
    }
    fn free(&mut self, frame: u64) -> Result<(), Error> {
        let i = self.held.iter().position(|&f| f == frame).expect("free of a frame not held");
        if self.hit(2, self.fail_free) {
            return Err(Error::Backend);
        }
        self.held.remove(i);
        self.freed += 1;
        Ok(())
    }
}

fn ready(pool: &mut Pool, f: &mut Fake, kind: Kind) -> Page {
    let page = pool.allocate(f, kind).unwrap();
    let page = pool.zero(f, page).unwrap();
    pool.prepare(&page).unwrap();
    page
}

#[test]
fn lifecycle_and_exact_accounting() {
    let (mut pool, mut f) = (Pool::new(1 << 32), Fake::new());
    let mut set: [Option<Page>; 4] = [None, None, None, None];
    pool.allocate_set(&mut f, &SET, &mut set).unwrap();
    assert_eq!(f.held.len(), 4);
    assert_eq!(f.zeroed.len(), 4, "every page is zeroed before it is prepared");
    let tokens: Vec<u64> = set.iter().map(|p| p.as_ref().unwrap().token()).collect();
    assert!(tokens.iter().all(|&t| t != 0));
    for (i, p) in set.iter().enumerate() {
        assert_eq!(p.as_ref().unwrap().kind(), SET[i]);
    }
    assert!((0..4).all(|s| pool.entry(s).unwrap().state == State::Prepared));
    let a = pool.accounting();
    assert_eq!((a.allocated, a.prepared, a.active_now), (4, 4, 4));
    assert_eq!(pool.cancel(&mut f, &mut set), 0);
    let a = pool.accounting();
    assert_eq!((a.released, a.active_now, a.retained_now, a.violations), (4, 0, 0, 0));
    assert!(f.held.is_empty() && f.freed == 4 && set.iter().all(Option::is_none));
    // Generations advance: a reused record issues a different token.
    let again = pool.allocate(&mut f, Kind::Command).unwrap();
    assert!(!tokens.contains(&again.token()));
    pool.release(&mut f, again).unwrap();
}

#[test]
fn budget_kind_and_closed_refusals() {
    let (mut pool, mut f) = (Pool::new(1 << 32), Fake::new());
    let pages: Vec<Page> = (0..ACTIVE_LIMIT).map(|_| ready(&mut pool, &mut f, Kind::Event)).collect();
    assert_eq!(pool.allocate(&mut f, Kind::Event).unwrap_err(), Error::Budget);
    assert_eq!(f.held.len(), usize::from(ACTIVE_LIMIT), "refusal acquires nothing");
    for kind in [Kind::Worker, Kind::Control, Kind::Mapping] {
        assert_eq!(pool.allocate(&mut f, kind).unwrap_err(), Error::Unsupported);
    }
    let a = pool.accounting();
    assert_eq!((a.allocation_failures, a.violations), (1, 3));
    for p in pages {
        pool.release(&mut f, p).unwrap();
    }
    assert!(f.held.is_empty());
}

#[test]
fn transitions_are_ordered() {
    let (mut pool, mut f) = (Pool::new(1 << 32), Fake::new());
    let page = pool.allocate(&mut f, Kind::Dcbaa).unwrap();
    assert_eq!(pool.prepare(&page), Err(Error::IllegalState)); // Allocated -> Prepared
    let page = pool.zero(&mut f, page).unwrap();
    let refused = pool.zero(&mut f, page).unwrap_err(); // Zeroed -> Zeroed
    assert_eq!(refused.error, Error::IllegalState);
    let page = refused.page.expect("refused page is handed back");
    pool.prepare(&page).unwrap();
    assert_eq!(pool.prepare(&page), Err(Error::IllegalState)); // twice
    assert_eq!(pool.accounting().violations, 3);
    pool.release(&mut f, page).unwrap();
    assert!(f.held.is_empty());
}

#[test]
fn stale_and_foreign_handles_are_refused() {
    // Same frame, same slot, older generation: only the generation differs.
    let frame = 0x30_0000;
    let mut fa = Fake::with(&[frame, frame]);
    let mut fb = Fake::with(&[frame]);
    let (mut a, mut b) = (Pool::new(1 << 32), Pool::new(1 << 32));
    let old = b.allocate(&mut fb, Kind::Erst).unwrap(); // slot 0, generation 1
    let first = a.allocate(&mut fa, Kind::Erst).unwrap();
    a.release(&mut fa, first).unwrap(); // slot 0 now generation 2
    let current = a.allocate(&mut fa, Kind::Erst).unwrap();
    let refused = a.release(&mut fa, old).unwrap_err();
    assert_eq!(refused.error, Error::Stale);
    let old = refused.page.unwrap();
    assert_eq!(a.accounting().violations, 1);
    assert_eq!(fa.held, vec![frame], "stale handle freed nothing");
    // Foreign handle with a different frame.
    let mut fc = Fake::new();
    let mut c = Pool::new(1 << 32);
    let foreign = c.allocate(&mut fc, Kind::Erst).unwrap();
    assert_eq!(a.quarantine(foreign, Reason::Deliberate).unwrap_err().error, Error::Stale);
    a.release(&mut fa, current).unwrap();
    b.release(&mut fb, old).unwrap();
    assert!(fa.held.is_empty() && fb.held.is_empty());
}

#[test]
fn frame_validation_and_rollback() {
    for (frame, limit) in [
        (0x20_0800u64, 1u64 << 32), // misaligned
        (0x8_0000, 1 << 32),        // below 1 MiB
        (FRAME_HIGH, 1 << 32),      // at the PMM limit
        (0x40_0000, 0x40_0000),     // beyond a small device limit
        (u64::MAX & !0xfff, 1 << 32),
    ] {
        let (mut pool, mut f) = (Pool::new(limit), Fake::with(&[frame]));
        assert_eq!(pool.allocate(&mut f, Kind::Command).unwrap_err(), Error::Bounds, "{frame:#x}");
        assert!(f.held.is_empty(), "unqualified frame returned");
        assert_eq!(pool.accounting().active_now, 0);
    }
    let (mut pool, mut f) = (Pool::new(1 << 32), Fake::with(&[FRAME_HIGH - 0x1000]));
    let last = pool.allocate(&mut f, Kind::Command).unwrap();
    pool.release(&mut f, last).unwrap();
    // A duplicate of a live frame is never accepted.
    let (mut pool, mut f) = (Pool::new(1 << 32), Fake::with(&[0x30_0000, 0x30_0000]));
    let held = pool.allocate(&mut f, Kind::Command).unwrap();
    f.held.push(0x30_0000); // the fake hands the same frame out again
    assert_eq!(pool.allocate(&mut f, Kind::Event).unwrap_err(), Error::Bounds);
    pool.release(&mut f, held).unwrap();
    // An unqualified frame that cannot be returned is retained, never reused.
    let (mut pool, mut f) = (Pool::new(1 << 32), Fake::with(&[0x8_0000]));
    f.fail_free = Some(0);
    assert_eq!(pool.allocate(&mut f, Kind::Command).unwrap_err(), Error::Bounds);
    let a = pool.accounting();
    assert_eq!((a.rollback_failures, a.retained_now, a.active_now), (1, 1, 0));
    assert_eq!(pool.entry(0).unwrap().reason, Reason::OutOfRange);
    assert_eq!(f.held, vec![0x8_0000]);
}

#[test]
fn partial_set_failures_at_every_step() {
    for step in 0..4u32 {
        // Allocation failure: every earlier page released, nothing kept.
        let (mut pool, mut f) = (Pool::new(1 << 32), Fake::new());
        f.fail_alloc = Some(step);
        let mut out: [Option<Page>; 4] = [None, None, None, None];
        assert_eq!(pool.allocate_set(&mut f, &SET, &mut out), Err(Error::Allocation));
        assert!(out.iter().all(Option::is_none) && f.held.is_empty());
        assert_eq!(pool.accounting().released, step);
        // Zeroing failure: that page's content is unknown, so it is retained;
        // the earlier pages are released.
        let (mut pool, mut f) = (Pool::new(1 << 32), Fake::new());
        f.fail_zero = Some(step);
        let mut out: [Option<Page>; 4] = [None, None, None, None];
        assert_eq!(pool.allocate_set(&mut f, &SET, &mut out), Err(Error::Quarantined));
        assert!(out.iter().all(Option::is_none));
        assert_eq!(f.held.len(), 1, "only the unzeroed frame is kept");
        let a = pool.accounting();
        assert_eq!((a.active_now, a.retained_now, a.released), (0, 1, step));
        let retained = (0..RECORDS).filter_map(|s| pool.entry(s)).find(|e| e.state == State::Quarantined).unwrap();
        assert_eq!((retained.reason, retained.frame), (Reason::ZeroFailed, f.held[0]));
    }
    // A release failure during rollback retains that page and is counted.
    let (mut pool, mut f) = (Pool::new(1 << 32), Fake::new());
    f.fail_alloc = Some(3);
    f.fail_free = Some(0);
    let mut out: [Option<Page>; 4] = [None, None, None, None];
    assert_eq!(pool.allocate_set(&mut f, &SET, &mut out), Err(Error::Allocation));
    let a = pool.accounting();
    assert_eq!((a.rollback_failures, a.retained_now, a.active_now, a.released), (1, 1, 0, 2));
    assert_eq!(f.held.len(), 1);
    // A caller-supplied non-empty output is refused up front.
    let (mut pool, mut f) = (Pool::new(1 << 32), Fake::new());
    let page = pool.allocate(&mut f, Kind::Command).unwrap();
    let mut busy: [Option<Page>; 4] = [Some(page), None, None, None];
    assert_eq!(pool.allocate_set(&mut f, &SET, &mut busy), Err(Error::Bounds));
    assert_eq!(pool.cancel(&mut f, &mut busy), 0);
}

#[test]
fn quarantine_is_bounded_and_fails_closed() {
    let (mut pool, mut f) = (Pool::new(1 << 32), Fake::new());
    for _ in 0..QUARANTINE_LIMIT {
        let page = ready(&mut pool, &mut f, Kind::Event);
        pool.quarantine(page, Reason::Deliberate).unwrap();
    }
    assert_eq!(pool.accounting().retained_now, QUARANTINE_LIMIT);
    assert!(!pool.closed());
    // Exhaustion: the page is still owned (Retained) and the pool closes.
    let page = ready(&mut pool, &mut f, Kind::Event);
    let refused = pool.quarantine(page, Reason::Deliberate).unwrap_err();
    assert!(refused.page.is_none() && refused.error == Error::Budget);
    assert!(pool.closed());
    assert_eq!(pool.allocate(&mut f, Kind::Event).unwrap_err(), Error::Quarantined);
    let states: Vec<State> = (0..RECORDS).filter_map(|s| pool.entry(s)).map(|e| e.state).collect();
    assert_eq!(states.iter().filter(|&&s| s == State::Quarantined).count(), 4);
    assert_eq!(states.iter().filter(|&&s| s == State::Retained).count(), 1);
    assert_eq!((f.held.len(), f.freed), (5, 0), "retained frames are never freed or reused");
    let a = pool.accounting();
    assert_eq!((a.retained_now, a.active_now, a.quarantined), (5, 0, 5));
}

#[test]
fn cancellation_releases_only_unpublished_pages() {
    let (mut pool, mut f) = (Pool::new(1 << 32), Fake::new());
    let kept = ready(&mut pool, &mut f, Kind::Erst);
    pool.quarantine(kept, Reason::Deliberate).unwrap();
    let a = pool.allocate(&mut f, Kind::Command).unwrap(); // Allocated
    let b = pool.allocate(&mut f, Kind::Event).unwrap();
    let b = pool.zero(&mut f, b).unwrap(); // Zeroed
    let c = ready(&mut pool, &mut f, Kind::Dcbaa); // Prepared
    f.fail_free = Some(1); // the second release (Zeroed page) fails
    let mut pages = [Some(a), Some(b), Some(c), None];
    assert_eq!(pool.cancel(&mut f, &mut pages), 1);
    let acct = pool.accounting();
    assert_eq!((acct.active_now, acct.retained_now, acct.released, acct.rollback_failures), (0, 2, 2, 1));
    assert_eq!(f.held.len(), 2, "quarantined page and the failed release stay owned");
    assert!(pages.iter().all(Option::is_none));
}

#[test]
fn device_limit_never_exceeds_pmm_range() {
    let (mut pool, mut f) = (Pool::new(u64::MAX), Fake::with(&[FRAME_HIGH]));
    assert_eq!(pool.allocate(&mut f, Kind::Command).unwrap_err(), Error::Bounds);
    assert!(Pool::new(0).accounting() == Default::default());
}
