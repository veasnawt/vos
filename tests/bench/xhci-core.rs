//! Optimized HOST logical round trips only; no DMA, device or hardware timing.
#![forbid(unsafe_code)]
use std::hint::black_box;
use std::mem::size_of;
use std::time::Instant;
use vos_xhci::xhci::PAGE;
use vos_xhci::xhci::capabilities::Capabilities;
use vos_xhci::xhci::controller::{Controller, PermanentSnapshot};
use vos_xhci::xhci::resources::Usage;
use vos_xhci::xhci::ring::{CommandRing, Event, EventRing, Region, Trb};

fn main() {
    let count = 200_000u64;
    let mut samples = [0u128; 5];
    let mut checksum = 0;
    for sample in &mut samples {
        let region = Region::new(0x101000, PAGE, 1 << 30).unwrap();
        let mut commands = CommandRing::new(region, 1).unwrap();
        let mut events = EventRing::new(region);
        let start = Instant::now();
        for _ in 0..count {
            let submission = black_box(commands.prepare_noop().unwrap());
            let trb = black_box(Trb {
                parameter: submission.id.address(),
                status: 1 << 24,
                control: (33 << 10) | u32::from(events.cursor().1),
            });
            match events.inspect(trb, 8).unwrap().unwrap() {
                Event::Completion(c) => commands.complete(submission.id, 1, c).unwrap(),
                _ => unreachable!(),
            }
            events.advance().unwrap();
            checksum ^= black_box(submission.id.serial());
        }
        *sample = start.elapsed().as_nanos();
    }
    println!(
        "{{\"host_simulation_only\":true,\"iterations_per_sample\":{count},\"sample_total_ns\":{:?},\"checksum\":{checksum},\"sizes\":{{\"Controller\":{},\"PermanentSnapshot\":{},\"Capabilities\":{},\"CommandRing\":{},\"EventRing\":{},\"Usage\":{},\"Trb\":{}}}}}",
        samples,
        size_of::<Controller>(),
        size_of::<PermanentSnapshot>(),
        size_of::<Capabilities>(),
        size_of::<CommandRing>(),
        size_of::<EventRing>(),
        size_of::<Usage>(),
        size_of::<Trb>()
    );
}
