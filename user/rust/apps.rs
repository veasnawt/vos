//! Optional ABI v4 extension. Existing vos ABI v3 runtime remains unchanged.
#![no_std]
pub use vos::{Error,Result};
#[repr(C)]
#[derive(Clone,Copy)]
pub struct AppEntry {
    pub version:u32,pub size:u32,pub app_id:u64,pub next_cookie:u64,
    pub name_length:u32,pub flags:u32,pub name:[u8;16],
}
impl AppEntry {pub const ZERO:Self=Self{version:0,size:0,app_id:0,next_cookie:0,name_length:0,flags:0,name:[0;16]};}
const _:()={
    assert!(core::mem::size_of::<AppEntry>()==48);
    assert!(core::mem::offset_of!(AppEntry,version)==0);
    assert!(core::mem::offset_of!(AppEntry,size)==4);
    assert!(core::mem::offset_of!(AppEntry,app_id)==8);
    assert!(core::mem::offset_of!(AppEntry,next_cookie)==16);
    assert!(core::mem::offset_of!(AppEntry,name_length)==24);
    assert!(core::mem::offset_of!(AppEntry,flags)==28);
    assert!(core::mem::offset_of!(AppEntry,name)==32);
};
pub mod raw {
    use super::AppEntry;
    unsafe extern "C" {
        pub fn vos_app_next(cookie:u64,out:*mut AppEntry,capacity:u64)->i64;
        pub fn vos_app_run_foreground(app_id:u64,flags:u64)->i64;
    }
}
pub fn next(cookie:u64)->Result<Option<AppEntry>> {
    let mut entry=AppEntry::ZERO;
    let n=unsafe{raw::vos_app_next(cookie,&mut entry,48)};
    if n<0 {return Err(Error((-n) as isize));}
    if n==0 {return Ok(None);}
    let size=entry.name_length as usize;
    if n!=48 || entry.version!=1 || entry.size!=48 || entry.flags!=0 ||
       entry.app_id==0 || entry.next_cookie==0 || size==0 || size>15 ||
       entry.name[size..].iter().any(|x|*x!=0) {return Err(Error::IO);}
    Ok(Some(entry))
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum Completion {Exited(i32),Faulted(u32),Cancelled(u32)}
pub fn decode(value:i64)->Result<Completion> {
    if value<0 {return Err(Error((-value) as isize));}
    let n=value as u64;
    match n>>32 {
        0=>Ok(Completion::Exited(n as u32 as i32)),
        1=>Ok(Completion::Faulted(n as u32)),
        2=>Ok(Completion::Cancelled(n as u32)),
        _=>Err(Error::IO),
    }
}
pub fn run(app_id:u64)->Result<Completion> {
    decode(unsafe{raw::vos_app_run_foreground(app_id,0)})
}
