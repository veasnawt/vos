//! Private PS/2 driver bridge. C retains storage, controller and lease ownership.
#[path="../../../kernel/rust/input/ps2_routed.rs"]
pub mod routing;
use routing::{Queue, Route, Packet, PointerQueue};
use routing::{pointer_poll, pointer_lost_and_clear};
use core::{mem::{align_of,offset_of,size_of},ptr};
#[repr(C)]
struct Io {input:unsafe extern "C" fn(u16)->u8,output:*const ()}
#[repr(C)]
struct Keyboard {
 io:*const Io,decoder:[u8;7],ready:i32,pointer_ready:i32,pending:i32,
 pointer:[u8;5],packet:Packet,lost:u32,queue:Queue,
}
const _:()={
 assert!(size_of::<Keyboard>()==88 && align_of::<Keyboard>()==8);
 assert!(size_of::<Io>()==16 && size_of::<Queue>()==40);
 assert!(offset_of!(Keyboard,io)==0 && offset_of!(Keyboard,decoder)==8);
 assert!(offset_of!(Keyboard,ready)==16 && offset_of!(Keyboard,pointer_ready)==20);
 assert!(offset_of!(Keyboard,pending)==24 && offset_of!(Keyboard,pointer)==28);
 assert!(offset_of!(Keyboard,packet)==34 && offset_of!(Keyboard,lost)==40);
 assert!(offset_of!(Keyboard,queue)==44 && offset_of!(Queue,head)==0);
 assert!(offset_of!(Queue,count)==4 && offset_of!(Queue,keys)==8);
};
unsafe extern "C" {
 fn keyboard_decode(state:*mut u8,byte:u8)->core::ffi::c_char;
 fn pointer_decode(state:*mut u8,byte:u8,packet:*mut u8)->i32;
 fn pointer_idle(state:*mut u8)->i32;
}
// Keep the accepted pure pointer decoder's register pressure out of the driver
// loop. A raw address passed by value avoids spilling an addressable Raw wrapper.
#[inline(never)]
unsafe fn pointer_step(k:*mut Keyboard,byte:u8)->i32 {
 // SAFETY: disjoint initialized decoder and aligned packet in the live C object.
 unsafe{pointer_decode(ptr::addr_of_mut!((*k).pointer).cast(),byte,ptr::addr_of_mut!((*k).packet).cast())}
}
struct Raw(*mut Keyboard);
// SAFETY for the accesses below: entry's live initialized exclusive C object
// contract. Raw holds no pointee reference. Any short field borrow ends before
// read() calls C. Reload the io pointer for each call, preserving callback-visible
// mutations; never copy/restore a stale whole object or read packet padding.
impl Route for Raw {
 #[inline(always)] fn pointer_pending(&self)->i32 {unsafe{ptr::addr_of!((*self.0).pending).read()}}
 #[inline(always)] fn key_count(&self)->u32 {unsafe{ptr::addr_of!((*self.0).queue.count).read()}}
 #[inline(always)] fn read(&mut self,port:u16)->u8 {
  // SAFETY: caller supplies valid io/in when read is reached. No field reference
  // or cached io pointer spans the foreign call; callback may change live fields
  // or substitute another valid io, but cannot free/unmap the keyboard.
  unsafe{let io=ptr::addr_of!((*self.0).io).read();let f=ptr::addr_of!((*io).input).read();f(port)}
 }
 #[inline(always)] fn reset_pointer(&mut self) {unsafe{ptr::addr_of_mut!((*self.0).pointer).write([0;5])}}
 #[inline(always)] fn decode_pointer(&mut self,byte:u8)->i32 {
  // SAFETY: entry contract; helper receives only the raw object address by value.
  unsafe{pointer_step(self.0,byte)}
 }
 #[inline(always)] fn lost(&mut self)->&mut u32 {unsafe{&mut *ptr::addr_of_mut!((*self.0).lost)}}
 #[inline(always)] fn set_pointer_pending(&mut self) {unsafe{ptr::addr_of_mut!((*self.0).pending).write(1)}}
 #[inline(always)] fn reset_keyboard(&mut self) {unsafe{ptr::addr_of_mut!((*self.0).decoder).write([0;7])}}
 #[inline(always)] fn decode_keyboard(&mut self,byte:u8)->u8 {
  // SAFETY: seven initialized bytes, exclusive only for this pure call.
  unsafe{keyboard_decode(ptr::addr_of_mut!((*self.0).decoder).cast(),byte) as u8}
 }
 #[inline(always)] fn queue(&mut self)->&mut Queue {unsafe{&mut *ptr::addr_of_mut!((*self.0).queue)}}
}
/// # Safety
/// k identifies a live, aligned, initialized 88-byte C keyboard for this entire
/// synchronous call. Existing C thread/IF=0 ownership rules apply. If reads are
/// reached, io/in are live valid synchronous callbacks. Callbacks may mutate
/// fields but must preserve live initialized storage and valid callback pointers;
/// no concurrent writer or retained access beyond the existing C contract.
/// No controller write, ownership change, callback retention or allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_keyboard_routed_drain(k:*mut core::ffi::c_void) {
 if k.is_null() || !(k as usize).is_multiple_of(8) || (k as usize).checked_add(88).is_none() {return;}
 routing::drain(&mut Raw(k.cast()));
}


impl PointerQueue for Raw {
    #[inline(always)]
    fn is_ready(&self) -> bool {
        unsafe { ptr::addr_of!((*self.0).ready).read() != 0 }
    }
    #[inline(always)]
    fn is_pointer_ready(&self) -> bool {
        unsafe { ptr::addr_of!((*self.0).pointer_ready).read() != 0 }
    }
    #[inline(always)]
    fn drain(&mut self) {
        unsafe { vos_rust_keyboard_routed_drain(self.0.cast()) }
    }
    #[inline(always)]
    fn poll_idle(&mut self) -> bool {
        unsafe { pointer_idle(ptr::addr_of_mut!((*self.0).pointer).cast()) != 0 }
    }
    #[inline(always)]
    fn take_packet(&mut self) -> Option<Packet> {
        unsafe {
            if ptr::addr_of!((*self.0).pending).read() != 0 {
                let pkt = ptr::addr_of!((*self.0).packet).read();
                ptr::addr_of_mut!((*self.0).pending).write(0);
                Some(pkt)
            } else {
                None
            }
        }
    }
}

/// # Safety
/// k and p identify live, aligned C storage for this entire synchronous call.
/// C thread/IF=0 ownership rules apply. Disjoint valid pointers required.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_keyboard_pointer_poll(
    k: *mut core::ffi::c_void,
    p: *mut core::ffi::c_void,
) -> core::ffi::c_int {
    if k.is_null()
        || p.is_null()
        || !(k as usize).is_multiple_of(8)
        || !(p as usize).is_multiple_of(2)
        || (k as usize).checked_add(88).is_none()
        || (p as usize).checked_add(6).is_none()
    {
        return 0;
    }
    let mut raw = Raw(k.cast());
    match pointer_poll(&mut raw) {
        Some(packet) => {
            unsafe {
                ptr::addr_of_mut!(*(p as *mut Packet)).write(packet);
            }
            1
        }
        None => 0,
    }
}

/// # Safety
/// k identifies live, aligned 88-byte C keyboard storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_keyboard_pointer_lost(k: *mut core::ffi::c_void) -> core::ffi::c_uint {
    if k.is_null() || !(k as usize).is_multiple_of(8) || (k as usize).checked_add(88).is_none() {
        return 0;
    }
    let mut raw = Raw(k.cast());
    pointer_lost_and_clear(raw.lost())
}

impl routing::KeyboardQueue for Raw {
    #[inline(always)] fn keyboard_ready(&self) -> bool {
        // Existing initialized ready field; no foreign callback or escaping borrow.
        <Self as PointerQueue>::is_ready(self)
    }
    #[inline(always)] fn fill_keyboard_queue(&mut self) {
        // Existing synchronous drain. Raw contains only an address; no pointee
        // reference spans the C input callback and all queue fields reload after it.
        <Self as PointerQueue>::drain(self)
    }
    #[inline(always)] fn keyboard_queue(&mut self) -> &mut Queue {
        <Self as Route>::queue(self)
    }
}
/// # Safety
/// k identifies live initialized, aligned caller-owned 88-byte keyboard storage
/// for this synchronous call. Existing thread/IF=0, io callback and exclusive
/// ownership contracts apply; callbacks cannot free/unmap storage. No reference
/// is retained across drain, no new controller write or ownership transition.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_keyboard_routed_poll(k:*mut core::ffi::c_void)->core::ffi::c_char {
    if !routing::keyboard_range_valid(k as usize) {return 0;}
    routing::keyboard_poll(&mut Raw(k.cast())) as core::ffi::c_char
}
