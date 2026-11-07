//! Private PS/2 driver bridge. C retains storage, controller and lease ownership.
#[path="../../../kernel/rust/input/ps2_routed.rs"]
pub mod routing;
#[path="../../../kernel/rust/input/ps2_ordinary.rs"]
pub mod ordinary;
#[path="../../../kernel/rust/input/ps2_controller_read.rs"]
pub mod controller_read;
#[path="../../../kernel/rust/input/ps2_controller_write.rs"]
pub mod controller_write;
#[path="../../../kernel/rust/input/ps2_lifecycle.rs"]
pub mod lifecycle;
use routing::{Queue, Route, Packet, PointerQueue};
use routing::{pointer_poll, pointer_lost_and_clear};
use core::{mem::{align_of,offset_of,size_of},ptr};
#[repr(C)]
struct Io {input:unsafe extern "C" fn(u16)->u8,output:Option<unsafe extern "C" fn(u16,u8)>}
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

// R2 ordinary keyboard_poll: reuses the Raw accessors above. ready is a plain
// field read; read() reloads io/in for every port access; the decoder reset and
// decode borrow only the seven decoder bytes and end before the next callback.
impl ordinary::Ordinary for Raw {
    #[inline(always)] fn ready(&self) -> bool {
        // SAFETY: entry contract; initialized int field read, no reference kept.
        unsafe { ptr::addr_of!((*self.0).ready).read() != 0 }
    }
    #[inline(always)] fn read(&mut self, port: u16) -> u8 { <Self as Route>::read(self, port) }
    #[inline(always)] fn reset_decoder(&mut self) { <Self as Route>::reset_keyboard(self) }
    #[inline(always)] fn decode(&mut self, byte: u8) -> u8 { <Self as Route>::decode_keyboard(self, byte) }
}
/// # Safety
/// k identifies live initialized, aligned caller-owned 88-byte keyboard storage
/// for this synchronous call, selected by the C dispatcher with pointer_ready==0.
/// Existing thread/IF=0, io callback and exclusive ownership contracts apply;
/// callbacks may mutate fields or substitute a valid io but cannot free/unmap
/// storage. No controller write, queue access, ownership change or allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_keyboard_ordinary_poll(k:*mut core::ffi::c_void)->core::ffi::c_char {
    if !routing::keyboard_range_valid(k as usize) {return 0;}
    ordinary::poll(&mut Raw(k.cast())) as core::ffi::c_char
}

// R2 bounded controller reads: same Raw read accessor (io/in reloaded per call).
impl controller_read::ControllerRead for Raw {
    #[inline(always)] fn read(&mut self, port: u16) -> u8 { <Self as Route>::read(self, port) }
}
#[inline(always)]
unsafe fn controller_read_entry(k:*mut core::ffi::c_void, value:*mut u8, channel:controller_read::Channel)->core::ffi::c_int {
    if !routing::keyboard_range_valid(k as usize) || value.is_null() {return 0;}
    match controller_read::read_channel(&mut Raw(k.cast()), channel) {
        Some(byte) => {
            // SAFETY: caller supplies a writable byte, disjoint from the keyboard
            // object; written only on success after the last callback returned.
            unsafe { value.write(byte) };
            1
        }
        None => 0,
    }
}
/// # Safety
/// k identifies live initialized, aligned caller-owned 88-byte keyboard storage
/// for this synchronous call and value a writable byte disjoint from it. Existing
/// thread/IF=0, io callback and exclusive ownership contracts apply; callbacks may
/// substitute a valid io but cannot free/unmap storage. No controller write.
#[unsafe(no_mangle)]
#[inline(never)]
pub unsafe extern "C" fn vos_rust_keyboard_read_keyboard(k:*mut core::ffi::c_void, value:*mut u8)->core::ffi::c_int {
    unsafe { controller_read_entry(k, value, controller_read::Channel::Keyboard) }
}
/// # Safety
/// Same contract as `vos_rust_keyboard_read_keyboard`, for the auxiliary channel.
#[unsafe(no_mangle)]
#[inline(never)]
pub unsafe extern "C" fn vos_rust_keyboard_read_aux(k:*mut core::ffi::c_void, value:*mut u8)->core::ffi::c_int {
    unsafe { controller_read_entry(k, value, controller_read::Channel::Aux) }
}

// R2-W bounded controller writes. The accepted C helpers' writes only: Port admits
// 0x60/0x64. io is reloaded for every status read and for the write itself, so a
// callback-substituted io is honoured exactly as the accepted C did.
impl controller_write::ControllerWrite for Raw {
    #[inline(always)] fn write(&mut self, port: u16, value: u8) -> bool {
        // SAFETY: entry contract as read(): live io table; no field reference or
        // cached io pointer spans the foreign call. A null out is refused, not called.
        unsafe {
            let io = ptr::addr_of!((*self.0).io).read();
            match ptr::addr_of!((*io).output).read() {
                Some(out) => { out(port, value); true }
                None => false,
            }
        }
    }
}
/// # Safety
/// Same object, io and exclusive-ownership contract as `vos_rust_keyboard_read_keyboard`.
/// Writes at most one byte, only to 0x60 or 0x64; any other port is refused without access.
#[unsafe(no_mangle)]
#[inline(never)]
pub unsafe extern "C" fn vos_rust_keyboard_write_port(k:*mut core::ffi::c_void, port:u16, value:u8)->core::ffi::c_int {
    if !routing::keyboard_range_valid(k as usize) {return 0;}
    match controller_write::Port::from_address(port) {
        Some(port) => controller_write::write_port(&mut Raw(k.cast()), port, value) as core::ffi::c_int,
        None => 0,
    }
}
/// # Safety
/// Same contract as `vos_rust_keyboard_write_port`; writes the byte to 0x60 and
/// requires keyboard acknowledgement 0xFA.
#[unsafe(no_mangle)]
#[inline(never)]
pub unsafe extern "C" fn vos_rust_keyboard_command(k:*mut core::ffi::c_void, value:u8)->core::ffi::c_int {
    if !routing::keyboard_range_valid(k as usize) {return 0;}
    controller_write::command(&mut Raw(k.cast()), value) as core::ffi::c_int
}
/// # Safety
/// Same contract as `vos_rust_keyboard_write_port`; writes 0xD4 to 0x64, the byte to
/// 0x60, and requires auxiliary acknowledgement 0xFA.
#[unsafe(no_mangle)]
#[inline(never)]
pub unsafe extern "C" fn vos_rust_keyboard_aux_command(k:*mut core::ffi::c_void, value:u8)->core::ffi::c_int {
    if !routing::keyboard_range_valid(k as usize) {return 0;}
    controller_write::aux_command(&mut Raw(k.cast()), value) as core::ffi::c_int
}

// R2-L controller lifecycle. Life carries the live object and, for init only, the
// caller's io table (the accepted C validates and drains through that table, while
// its helpers use the object's io). Controller accesses use the accepted bounded
// entries above, out of line; state fields are written in place at the accepted
// points relative to the callbacks. Same entry contract as the helpers.
struct Life {k:*mut Keyboard,io:*const Io}
impl lifecycle::Lifecycle for Life {
    #[inline(always)] fn write_port(&mut self, port: controller_write::Port, value: u8) -> bool {
        unsafe { vos_rust_keyboard_write_port(self.k.cast(), port.address(), value) != 0 }
    }
    #[inline(always)] fn command(&mut self, value: u8) -> bool { unsafe { vos_rust_keyboard_command(self.k.cast(), value) != 0 } }
    #[inline(always)] fn aux_command(&mut self, value: u8) -> bool { unsafe { vos_rust_keyboard_aux_command(self.k.cast(), value) != 0 } }
    #[inline(always)] fn read_keyboard(&mut self) -> Option<u8> {
        let mut byte = 0u8;
        // SAFETY: byte is a live local disjoint from the keyboard object.
        if unsafe { vos_rust_keyboard_read_keyboard(self.k.cast(), &mut byte) } != 0 { Some(byte) } else { None }
    }
    #[inline(always)] fn read_aux(&mut self) -> Option<u8> {
        let mut byte = 0u8;
        if unsafe { vos_rust_keyboard_read_aux(self.k.cast(), &mut byte) } != 0 { Some(byte) } else { None }
    }
    #[inline(always)] fn read(&mut self, port: u16) -> u8 { <Raw as Route>::read(&mut Raw(self.k), port) }
    #[inline(always)] fn suspend(&mut self) -> bool { unsafe { vos_rust_keyboard_suspend(self.k.cast()) != 0 } }
    #[inline(always)] fn resume(&mut self) -> bool { unsafe { vos_rust_keyboard_resume(self.k.cast()) != 0 } }
    // SAFETY (field accessors): entry contract; plain in-place loads/stores, no
    // reference retained across any callback.
    #[inline(always)] fn io_present(&self) -> bool { unsafe { !ptr::addr_of!((*self.k).io).read().is_null() } }
    #[inline(always)] fn ready(&self) -> bool { unsafe { ptr::addr_of!((*self.k).ready).read() != 0 } }
    #[inline(always)] fn set_ready(&mut self, ready: bool) { unsafe { ptr::addr_of_mut!((*self.k).ready).write(ready as i32) } }
    #[inline(always)] fn pointer_ready(&self) -> bool { unsafe { ptr::addr_of!((*self.k).pointer_ready).read() != 0 } }
    #[inline(always)] fn set_pointer_ready(&mut self, ready: bool) { unsafe { ptr::addr_of_mut!((*self.k).pointer_ready).write(ready as i32) } }
    #[inline(always)] fn clear_decoder(&mut self) { unsafe { ptr::addr_of_mut!((*self.k).decoder).write([0; 7]) } }
    #[inline(always)] fn clear_pointer_decoder(&mut self) { <Raw as Route>::reset_pointer(&mut Raw(self.k)) }
    #[inline(always)] fn clear_pointer_pending(&mut self) { unsafe { ptr::addr_of_mut!((*self.k).pending).write(0) } }
    #[inline(always)] fn clear_pointer_lost(&mut self) { unsafe { ptr::addr_of_mut!((*self.k).lost).write(0) } }
    #[inline(always)] fn clear_key_head(&mut self) { unsafe { ptr::addr_of_mut!((*self.k).queue.head).write(0) } }
    #[inline(always)] fn clear_key_count(&mut self) { unsafe { ptr::addr_of_mut!((*self.k).queue.count).write(0) } }
}
impl lifecycle::Init for Life {
    #[inline(always)] fn reset_object(&mut self) {
        // SAFETY: k is valid, aligned, writable 88-byte storage (entry contract);
        // every byte (padding included) is zeroed, then io stored, as the accepted C.
        unsafe { ptr::write_bytes(self.k, 0, 1); ptr::addr_of_mut!((*self.k).io).write(self.io) }
    }
    #[inline(always)] fn io_complete(&self) -> bool {
        // SAFETY: the table is read only when non-null; the input slot is read as raw
        // bits so a null callback is detected without forming a null fn value.
        !self.io.is_null() && unsafe {
            ptr::addr_of!((*self.io).input).cast::<usize>().read() != 0 && ptr::addr_of!((*self.io).output).read().is_some()
        }
    }
    #[inline(always)] fn read_original(&mut self, port: u16) -> u8 {
        // SAFETY: io_complete() held before any read; the caller's table is not
        // reloaded from the object, exactly as the accepted C drain.
        unsafe { let f = ptr::addr_of!((*self.io).input).read(); f(port) }
    }
}
/// # Safety
/// k identifies writable, aligned, caller-owned 88-byte keyboard storage for this
/// synchronous call (its prior contents are discarded); io is null or a callback
/// table that stays valid. Thread/IF=0 ownership as keyboard_poll. Issues only the
/// accepted initialization sequence; no output-port, A20, reset or PIC commands.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_keyboard_init(k:*mut core::ffi::c_void, io:*const core::ffi::c_void)->core::ffi::c_int {
    if !routing::keyboard_range_valid(k as usize) {return 0;}
    lifecycle::init(&mut Life{k:k.cast(),io:io.cast()}) as core::ffi::c_int
}
/// # Safety
/// Same live object, io and ownership contract as `vos_rust_keyboard_read_keyboard`.
#[unsafe(no_mangle)]
#[inline(never)]
pub unsafe extern "C" fn vos_rust_keyboard_suspend(k:*mut core::ffi::c_void)->core::ffi::c_int {
    if !routing::keyboard_range_valid(k as usize) {return 0;}
    lifecycle::suspend(&mut Life{k:k.cast(),io:ptr::null()}) as core::ffi::c_int
}
/// # Safety
/// Same contract as `vos_rust_keyboard_suspend`.
#[unsafe(no_mangle)]
#[inline(never)]
pub unsafe extern "C" fn vos_rust_keyboard_resume(k:*mut core::ffi::c_void)->core::ffi::c_int {
    if !routing::keyboard_range_valid(k as usize) {return 0;}
    lifecycle::resume(&mut Life{k:k.cast(),io:ptr::null()}) as core::ffi::c_int
}
/// # Safety
/// Same contract as `vos_rust_keyboard_suspend`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_keyboard_pointer_start(k:*mut core::ffi::c_void)->core::ffi::c_int {
    if !routing::keyboard_range_valid(k as usize) {return 0;}
    lifecycle::pointer_start(&mut Life{k:k.cast(),io:ptr::null()}) as core::ffi::c_int
}
/// # Safety
/// Same contract as `vos_rust_keyboard_suspend`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_keyboard_pointer_stop(k:*mut core::ffi::c_void) {
    if !routing::keyboard_range_valid(k as usize) {return;}
    lifecycle::pointer_stop(&mut Life{k:k.cast(),io:ptr::null()})
}
