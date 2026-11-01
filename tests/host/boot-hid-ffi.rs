//! Test-only unsafe C bridge. This file is NOT linked into VOS; production core forbids unsafe.
#![no_std]
extern crate boot_hid as hid;
use hid::*;
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe extern "C" {
        fn abort() -> !;
    }
    unsafe { abort() }
}
const D: [u8; 18] = [18, 1, 0, 2, 0, 0, 0, 64, 1, 0, 2, 0, 0, 1, 0, 0, 0, 1];
const C: [u8; 59] = [
    9, 2, 59, 0, 2, 1, 0, 128, 50, 9, 4, 0, 0, 1, 3, 1, 1, 0, 9, 33, 17, 1, 0, 1, 34, 63, 0, 7, 5,
    129, 3, 8, 0, 10, 9, 4, 1, 0, 1, 3, 1, 2, 0, 9, 33, 17, 1, 0, 1, 34, 63, 0, 7, 5, 130, 3, 8, 0,
    10,
];
// C owns 2048 aligned initialized bytes, and uses each context exclusively. All pointers below are test harness contracts.
const _: () =
    assert!(core::mem::size_of::<Session>() <= 2048 && core::mem::align_of::<Session>() <= 16);
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hid_init(p: *mut Session, mouse: u32) {
    let b = Selection::parse(Device::parse(&D, Speed::Full).unwrap(), &C).unwrap();
    unsafe {
        p.write(
            Session::new(
                if mouse == 0 { b.keyboard } else { b.mouse }.unwrap(),
                u64::from(mouse) + 1,
                1,
            )
            .unwrap(),
        )
    };
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hid_focus(p: *mut Session, owner: u64, epoch: u64) -> i32 {
    if unsafe { &mut *p }.focus(Focus { owner, epoch }).is_ok() {
        0
    } else {
        -1
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hid_revoke(p: *mut Session, epoch: u64) -> i32 {
    if unsafe { &mut *p }.revoke(epoch).is_ok() {
        0
    } else {
        -1
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hid_confirm(p: *mut Session) -> i32 {
    let s = unsafe { &mut *p };
    match s.request_protocol() {
        Ok(r) => {
            if s.confirm_protocol(r, Completion::Success).is_ok() {
                0
            } else {
                -1
            }
        }
        Err(_) => -1,
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hid_report(p: *mut Session, b: *const u8, n: usize) -> i32 {
    if n > 8 {
        return -1;
    }
    if unsafe { &mut *p }
        .report(unsafe { core::slice::from_raw_parts(b, n) })
        .is_ok()
    {
        0
    } else {
        -1
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hid_take(p: *mut Session, owner: u64, epoch: u64, out: *mut i32) -> i32 {
    let r = unsafe { &mut *p }.take(Focus { owner, epoch });
    let fields = match r {
        Ok(Some(Event::Reset)) => [3, 0, 0, 0],
        Ok(Some(Event::Key { usage, down })) => [1, i32::from(usage), i32::from(down), 0],
        Ok(Some(Event::Pointer { dx, dy, buttons })) => {
            [2, i32::from(dx), i32::from(dy), i32::from(buttons)]
        }
        Ok(None) => return 0,
        Err(_) => return -1,
    };
    for (i, v) in fields.into_iter().enumerate() {
        unsafe { out.add(i).write(v) };
    }
    1
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hid_disconnect(p: *mut Session) {
    unsafe { &mut *p }.disconnect();
}
