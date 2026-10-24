use super::*;
use core::sync::atomic::{AtomicIsize,AtomicUsize,Ordering};
static RETURN:AtomicIsize=AtomicIsize::new(0);
static WRITES:AtomicUsize=AtomicUsize::new(0);
fn status()->isize {RETURN.load(Ordering::SeqCst)}
#[unsafe(no_mangle)] extern "C" fn vos_exit(_:i32)->! {panic!("host tests must not exit through VOS")}
#[unsafe(no_mangle)] extern "C" fn vos_debug_write(_: *const u8,n:usize)->isize {if status()<0 {status()} else {n as isize}}
#[unsafe(no_mangle)] extern "C" fn vos_yield()->isize {status()}
#[unsafe(no_mangle)] extern "C" fn vos_handle_close(_:isize)->isize {status()}
#[unsafe(no_mangle)] extern "C" fn vos_channel_send(_:isize,_:*const u8,_:usize)->isize {status()}
#[unsafe(no_mangle)] extern "C" fn vos_channel_recv(_:isize,_:*mut u8,_:usize)->isize {status()}
#[unsafe(no_mangle)] extern "C" fn vos_file_initial(_:u64)->isize {status()}
#[unsafe(no_mangle)] extern "C" fn vos_file_open_at(_:isize,_:*const u8,_:usize,_:u64)->isize {status()}
#[unsafe(no_mangle)] extern "C" fn vos_file_read_at(_:isize,_:*mut u8,_:usize,_:u64)->isize {status()}
#[unsafe(no_mangle)] extern "C" fn vos_file_stat(_:isize,p:*mut u8,n:usize)->isize {
    assert_eq!(n,64);
    if status()==0 {unsafe {let s=&mut *p.cast::<file::Stat>();s.size=123;s.rights=3;}}
    status()
}
#[unsafe(no_mangle)] extern "C" fn vos_directory_next(_:isize,_:u64,p:*mut u8,n:usize)->isize {
    assert_eq!(n,272);
    if status()==1 {unsafe {let e=&mut *p.cast::<file::Entry>();e.next_cookie=17;e.kind=1;e.name_length=4;e.name[..4].copy_from_slice(b"test");}}
    status()
}
#[unsafe(no_mangle)] extern "C" fn vos_console_read(p:*mut u8,n:usize)->isize {
    if status()==2 && n>=3 {unsafe {core::ptr::copy_nonoverlapping(b"ok\0".as_ptr(),p,3)}}
    status()
}
#[unsafe(no_mangle)] extern "C" fn vos_console_write(_:*const u8,n:usize)->isize {
    assert!(n<=256);WRITES.fetch_add(1,Ordering::SeqCst);if status()<0 {status()} else {n as isize}
}
#[unsafe(no_mangle)] extern "C" fn vos_console_clear()->isize {status()}

#[test]
fn sdk_contract() {
    assert_eq!(ABI_VERSION,3);assert_eq!(core::mem::size_of::<file::Stat>(),64);
    assert_eq!(core::mem::size_of::<file::Entry>(),272);
    RETURN.store(-9,Ordering::SeqCst);
    assert_eq!(console::write(b"denied"),Err(Error::ACCESS));assert_eq!(file::initial(0),Err(Error::ACCESS));
    assert_eq!(channel::send(1,b""),Err(Error::ACCESS));assert_eq!(close(1),Err(Error::ACCESS));
    let mut input=[0x5Au8;4];assert_eq!(console::read_line(&mut input),Err(Error::ACCESS));assert_eq!(input,[0x5A;4]);
    RETURN.store(2,Ordering::SeqCst);assert_eq!(console::read_line(&mut input),Ok(2));assert_eq!(input,*b"ok\0Z");
    assert_eq!(channel::receive(1,&mut input),Ok(2));assert_eq!(file::read_at(1,&mut input,0),Ok(2));
    RETURN.store(5,Ordering::SeqCst);assert_eq!(console::read_line(&mut input),Err(Error::IO));
    assert_eq!(channel::receive(1,&mut input),Err(Error::IO));assert_eq!(file::read_at(1,&mut input,0),Err(Error::IO));
    RETURN.store(0,Ordering::SeqCst);assert_eq!(console::read_line(&mut[]),Err(Error::IO));
    WRITES.store(0,Ordering::SeqCst);assert_eq!(console::write_all(&[b'x';513]),Ok(()));assert_eq!(WRITES.load(Ordering::SeqCst),3);
    assert_eq!(debug_write(b"hello"),Ok(5));assert_eq!(yield_now(),Ok(()));assert_eq!(console::clear(),Ok(()));
    let s=file::stat(1).unwrap();assert_eq!(s.size,123);assert_eq!(s.rights,3);
    assert!(file::next(1,0).unwrap().is_none());RETURN.store(1,Ordering::SeqCst);
    let e=file::next(1,0).unwrap().unwrap();assert_eq!(e.next_cookie,17);assert_eq!(e.name(),b"test");
    RETURN.store(-8,Ordering::SeqCst);assert_eq!(console::read_line(&mut input),Err(Error::MESSAGE_SIZE));
}
