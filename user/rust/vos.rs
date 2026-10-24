//! Minimal freestanding VOS ABI v3 SDK. No allocation, TLS or implicit grants.
#![cfg_attr(not(test), no_std)]

pub const ABI_VERSION: u32 = 3;
pub const MESSAGE_MAX: usize = 256;
pub const CONSOLE_LINE_MAX: usize = 128;
pub const FILE_READ_MAX: usize = 4096;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Error(pub isize);
impl Error {
    pub const NOSYS: Self=Self(1);
    pub const INVALID: Self=Self(2);
    pub const FAULT: Self=Self(3);
    pub const AGAIN: Self=Self(4);
    pub const PIPE: Self=Self(5);
    pub const BAD_HANDLE: Self=Self(6);
    pub const NO_MEMORY: Self=Self(7);
    pub const MESSAGE_SIZE: Self=Self(8);
    pub const ACCESS: Self=Self(9);
    pub const NO_ENTRY: Self=Self(10);
    pub const NOT_DIRECTORY: Self=Self(11);
    pub const IS_DIRECTORY: Self=Self(12);
    pub const NAME_TOO_LONG: Self=Self(13);
    pub const LOOP: Self=Self(14);
    pub const IO: Self=Self(15);
    pub const UNAVAILABLE: Self=Self(16);
    pub const STALE: Self=Self(17);
    pub const TIMED_OUT: Self=Self(18);
    pub const INTERRUPTED: Self=Self(19);
    pub const UNSUPPORTED: Self=Self(20);
}
pub type Result<T> = core::result::Result<T,Error>;
fn result(n:isize)->Result<usize> {if n<0 {Err(Error(-n))} else {Ok(n as usize)}}
fn count(n:isize,capacity:usize)->Result<usize> {let n=result(n)?;if n>capacity {Err(Error::IO)} else {Ok(n)}}

/// Exact existing C ABI. Pointer validity and lengths remain the caller's
/// responsibility; prefer the slice-based wrappers below. No syscall numbers
/// are duplicated here: the same in-tree assembly stubs serve C and Rust.
pub mod raw {
    unsafe extern "C" {
        pub fn vos_exit(status:i32)->!;
        pub fn vos_debug_write(p:*const u8,len:usize)->isize;
        pub fn vos_yield()->isize;
        pub fn vos_channel_send(handle:isize,p:*const u8,len:usize)->isize;
        pub fn vos_channel_recv(handle:isize,p:*mut u8,capacity:usize)->isize;
        pub fn vos_handle_close(handle:isize)->isize;
        pub fn vos_file_open_at(base:isize,path:*const u8,len:usize,rights:u64)->isize;
        pub fn vos_file_read_at(file:isize,p:*mut u8,len:usize,offset:u64)->isize;
        pub fn vos_file_stat(handle:isize,p:*mut u8,size:usize)->isize;
        pub fn vos_directory_next(handle:isize,cookie:u64,p:*mut u8,size:usize)->isize;
        pub fn vos_file_initial(index:u64)->isize;
        pub fn vos_console_read(p:*mut u8,capacity:usize)->isize;
        pub fn vos_console_write(p:*const u8,len:usize)->isize;
        pub fn vos_console_clear()->isize;
    }
}
pub fn exit(status:i32)->! {unsafe {raw::vos_exit(status)}}
pub fn debug_write(data:&[u8])->Result<usize> {count(unsafe {raw::vos_debug_write(data.as_ptr(),data.len())},data.len())}
pub fn yield_now()->Result<()> {result(unsafe {raw::vos_yield()}).map(|_|())}
pub fn close(handle:isize)->Result<()> {result(unsafe {raw::vos_handle_close(handle)}).map(|_|())}

pub mod channel {
    use super::*;
    pub fn send(handle:isize,data:&[u8])->Result<()> {result(unsafe {raw::vos_channel_send(handle,data.as_ptr(),data.len())}).map(|_|())}
    pub fn receive(handle:isize,data:&mut[u8])->Result<usize> {count(unsafe {raw::vos_channel_recv(handle,data.as_mut_ptr(),data.len())},data.len())}
}
pub mod console {
    use super::*;
    /// Empty input is Ok(0) with a NUL at data[0]. Capacity includes that NUL.
    /// On MESSAGE_SIZE the kernel retains the completed line for retry.
    pub fn read_line(data:&mut[u8])->Result<usize> {
        let n=result(unsafe {raw::vos_console_read(data.as_mut_ptr(),data.len())})?;
        if n>=data.len() || data[n]!=0 {Err(Error::IO)} else {Ok(n)}
    }
    pub fn write(data:&[u8])->Result<usize> {count(unsafe {raw::vos_console_write(data.as_ptr(),data.len())},data.len())}
    pub fn write_all(data:&[u8])->Result<()> {
        for part in data.chunks(MESSAGE_MAX) {if write(part)?!=part.len() {return Err(Error::IO);}}
        Ok(())
    }
    pub fn clear()->Result<()> {result(unsafe {raw::vos_console_clear()}).map(|_|())}
}
pub mod file {
    use super::*;
    pub const READ:u64=1; pub const STAT:u64=2; pub const ENUMERATE:u64=4; pub const TRAVERSE:u64=8;
    #[repr(C)]
    #[derive(Clone,Copy,Default)]
    pub struct Stat {
        pub size:u64, pub blocks512:u64,
        pub kind:u32, pub mode:u32, pub uid:u32, pub gid:u32, pub links:u32, pub rights:u32,
        pub reserved:[u8;24],
    }
    #[repr(C)]
    pub struct Entry {pub next_cookie:u64,pub kind:u32,pub name_length:u32,pub name:[u8;256]}
    impl Entry {pub fn name(&self)->&[u8] {&self.name[..(self.name_length as usize).min(256)]}}
    const _:()=assert!(core::mem::size_of::<Stat>()==64);
    const _:()=assert!(core::mem::offset_of!(Stat,rights)==36);
    const _:()=assert!(core::mem::size_of::<Entry>()==272);
    const _:()=assert!(core::mem::offset_of!(Entry,name)==16);
    pub fn initial(index:u64)->Result<isize> {result(unsafe {raw::vos_file_initial(index)}).map(|n|n as isize)}
    pub fn open_at(base:isize,path:&[u8],rights:u64)->Result<isize> {result(unsafe {raw::vos_file_open_at(base,path.as_ptr(),path.len(),rights)}).map(|n|n as isize)}
    pub fn read_at(handle:isize,data:&mut[u8],offset:u64)->Result<usize> {count(unsafe {raw::vos_file_read_at(handle,data.as_mut_ptr(),data.len(),offset)},data.len())}
    pub fn stat(handle:isize)->Result<Stat> {
        let mut out=Stat::default();result(unsafe {raw::vos_file_stat(handle,(&mut out as *mut Stat).cast(),64)})?;Ok(out)
    }
    pub fn next(handle:isize,cookie:u64)->Result<Option<Entry>> {
        let mut out=Entry{next_cookie:0,kind:0,name_length:0,name:[0;256]};
        match result(unsafe {raw::vos_directory_next(handle,cookie,(&mut out as *mut Entry).cast(),272)})? {
            0=>Ok(None),1 if out.name_length<=256=>Ok(Some(out)),_=>Err(Error::IO),
        }
    }
}

/// Use with #![no_std] and #![no_main]. The existing assembly _start calls
/// this C-ABI main; its result becomes the normal process exit status.
#[macro_export]
macro_rules! entry {
    ($run:path)=>{#[unsafe(no_mangle)] pub extern "C" fn main()->i32 {$run()}};
}
#[cfg(not(test))]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo)->! {exit(99)}

#[cfg(test)]
#[path="../../tests/rust/sdk.rs"]
mod tests;
