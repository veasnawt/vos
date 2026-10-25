//! Hosted tests of the actual optional SDK module; mock only the raw syscall.
extern crate self as vos;
#[derive(Clone,Copy,Debug,PartialEq,Eq)] pub struct Error(pub isize);
impl Error {pub const IO:Self=Self(15);}
pub type Result<T>=core::result::Result<T,Error>;
#[path="../../user/rust/apps.rs"] mod apps;
use std::sync::atomic::{AtomicI64,AtomicU32,Ordering};
static NEXT:AtomicI64=AtomicI64::new(48);
static MODE:AtomicU32=AtomicU32::new(0);
static RUN:AtomicI64=AtomicI64::new(0);
#[unsafe(no_mangle)] extern "C" fn vos_app_next(_:u64,p:*mut apps::AppEntry,n:u64)->i64{
 assert_eq!(n,48);let r=NEXT.load(Ordering::SeqCst);
 if r==48 {unsafe{*p=apps::AppEntry{version:1,size:48,app_id:0x1002,next_cookie:18,name_length:5,flags:0,name:*b"hello\0\0\0\0\0\0\0\0\0\0\0"};}}
 if r==48 {unsafe {match MODE.load(Ordering::SeqCst) {1=>(*p).app_id=0,2=>(*p).next_cookie=0,3=>(*p).flags=1,4=>(*p).name_length=16,5=>(*p).name[15]=1,_=>{}}}}
 r
}
#[unsafe(no_mangle)] extern "C" fn vos_app_run_foreground(id:u64,flags:u64)->i64{
 assert_eq!(id,0x1002);assert_eq!(flags,0);RUN.load(Ordering::SeqCst)
}
#[test] fn layout(){assert_eq!(core::mem::size_of::<apps::AppEntry>(),48);assert_eq!(core::mem::align_of::<apps::AppEntry>(),8);}
#[test] fn completion(){
 use apps::Completion::*;
 assert_eq!(apps::decode(0),Ok(Exited(0)));
 assert_eq!(apps::decode(u32::MAX as i64-6),Ok(Exited(-7)));
 assert_eq!(apps::decode((1<<32)|14),Ok(Faulted(14)));
 assert_eq!(apps::decode((2<<32)|1),Ok(Cancelled(1)));
 assert_eq!(apps::decode(-7),Err(Error(7)));
 assert_eq!(apps::decode(3<<32),Err(Error::IO));
}
#[test] fn wrappers(){
 NEXT.store(48,Ordering::SeqCst);let e=apps::next(0).unwrap().unwrap();assert_eq!(e.app_id,0x1002);
 for (raw,expected) in [(0,None),(-3,Some(Error(3))),(47,Some(Error::IO))] {
  NEXT.store(raw,Ordering::SeqCst);let r=apps::next(0);assert_eq!(r.err(),expected);
 }
 for raw in [0,u32::MAX as i64-6,(1<<32)|14,(2<<32)|1,-19] {
  RUN.store(raw,Ordering::SeqCst);assert_eq!(apps::run(0x1002),apps::decode(raw));
 }
}

#[test] fn zero_and_invalid_records(){
 NEXT.store(48,Ordering::SeqCst);
 for mode in 1..=5 {MODE.store(mode,Ordering::SeqCst);assert_eq!(apps::next(0).err(),Some(Error::IO));}
 MODE.store(0,Ordering::SeqCst);
 assert!(apps::next(0).unwrap().is_some());
}
