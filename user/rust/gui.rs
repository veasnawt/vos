//! VOS ABI v5 GUI bindings. No allocator, device pointer, font or UI dependency.
#![no_std]
use vos::{Error,Result};
pub const MAX_WIDTH:u32=256;
pub const MAX_HEIGHT:u32=160;
pub const UPLOAD_MAX:usize=4096;
#[repr(C)]
#[derive(Clone,Copy,Default)]
pub struct Create {pub version:u32,pub size:u32,pub width:u32,pub height:u32,pub stride:u32,pub format:u32}
#[repr(C)]
#[derive(Clone,Copy,Default)]
pub struct Upload {pub x:u32,pub y:u32,pub width:u32,pub height:u32,pub stride:u32,pub length:u32,pub pixels:u64}
#[repr(C)]
#[derive(Clone,Copy,Default,Debug,PartialEq)]
pub struct Event {pub kind:u32,pub code:u32,pub sequence:u64}
const _:()=assert!(core::mem::size_of::<Create>()==24);
const _:()=assert!(core::mem::size_of::<Upload>()==32);
const _:()=assert!(core::mem::size_of::<Event>()==16);
pub mod raw {
    unsafe extern "C" {
        pub fn vos_gui_create(desc:*const super::Create,size:u64)->i64;
        pub fn vos_gui_upload(token:u64,desc:*const super::Upload,size:u64)->i64;
        pub fn vos_gui_present(token:u64,zero:u64,zero2:u64)->i64;
        pub fn vos_gui_release(token:u64,zero:u64,zero2:u64)->i64;
        pub fn vos_gui_pointer_state(event:*mut super::Event,size:u64,zero:u64)->i64;
        pub fn vos_gui_event(event:*mut super::Event,size:u64,flags:u64)->i64;
    }
}
fn result(n:i64)->Result<u64> {
    if n<0 {Err(Error((-n) as isize))}else{Ok(n as u64)}
}
fn empty(n:i64)->Result<()> {if result(n)?==0 {Ok(())}else{Err(Error::IO)}}
pub struct Surface {token:u64,pub width:u32,pub height:u32}
impl Surface {
    pub fn create(width:u32,height:u32)->Result<Self> {
        let stride=width.checked_mul(4).ok_or(Error::INVALID)?;
        let d=Create{version:1,size:24,width,height,stride,format:1};
        let token=result(unsafe{raw::vos_gui_create(&d,24)})?;
        if token==0{return Err(Error::IO)}
        Ok(Self{token,width,height})
    }
    pub fn upload(&self,x:u32,y:u32,width:u32,height:u32,stride:u32,pixels:&[u8])->Result<()> {
        if pixels.len()>UPLOAD_MAX{return Err(Error::INVALID)}
        let d=Upload{x,y,width,height,stride,length:pixels.len() as u32,pixels:pixels.as_ptr() as u64};
        empty(unsafe{raw::vos_gui_upload(self.token,&d,32)})
    }
    pub fn present(&self)->Result<()> {empty(unsafe{raw::vos_gui_present(self.token,0,0)})}
    pub fn release(mut self)->Result<()> {
        let n=unsafe{raw::vos_gui_release(self.token,0,0)};self.token=0;empty(n)
    }
}
impl Drop for Surface {fn drop(&mut self){if self.token!=0{let _=unsafe{raw::vos_gui_release(self.token,0,0)};}}}
pub fn event(block:bool)->Result<Event> {
    let mut e=Event::default();
    let count=result(unsafe{raw::vos_gui_event(&mut e,16,if block{0}else{1})})?;
    if count!=16 || !(e.kind==1 || e.kind==2){return Err(Error::IO)}
    Ok(e)
}

pub const POINTER_MOVE:u32=3;
pub const POINTER_BUTTON:u32=4;
pub const POINTER_SNAPSHOT:u32=5;
impl Event {
    pub fn x(self)->u32 {self.code&255}
    pub fn y(self)->u32 {(self.code>>8)&255}
    pub fn left(self)->bool {self.code&0x10000!=0}
    pub fn inside(self)->bool {self.code&0x20000!=0}
}
/// Requires an explicitly installed pointer grant; old event() remains key-only.
pub fn pointer_event(block:bool)->Result<Event> {
    let mut e=Event::default();
    let n=result(unsafe{raw::vos_gui_event(&mut e,16,if block{0}else{1})})?;
    if n!=16 || !(1..=4).contains(&e.kind){return Err(Error::IO)}
    Ok(e)
}
/// Snapshot is the overflow recovery barrier: discard queued sequence <= this.
pub fn pointer_state()->Result<Event> {
    let mut e=Event::default();
    let n=result(unsafe{raw::vos_gui_pointer_state(&mut e,16,0)})?;
    if n!=16 || e.kind!=5{return Err(Error::IO)}
    Ok(e)
}
