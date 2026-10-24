#![no_std]
#![no_main]

use vos::{Error,Result};
vos::entry!(run);
fn demonstrate()->Result<()> {
    vos::debug_write(b"Rust SDK example: independent ring-3 application\n")?;
    vos::yield_now()?;
    if vos::console::write(b"must not appear")!=Err(Error::ACCESS) {return Err(Error::IO);}
    let mut data=[0x5Au8;4];
    if vos::console::read_line(&mut data)!=Err(Error::ACCESS) || vos::console::clear()!=Err(Error::ACCESS) {return Err(Error::IO);}
    if vos::file::initial(0)!=Err(Error::BAD_HANDLE) {return Err(Error::IO);}
    if vos::file::open_at(999,b".",vos::file::READ)!=Err(Error::BAD_HANDLE) {return Err(Error::IO);}
    if vos::file::read_at(999,&mut data,0)!=Err(Error::BAD_HANDLE) {return Err(Error::IO);}
    if !matches!(vos::file::stat(999),Err(Error::BAD_HANDLE)) || !matches!(vos::file::next(999,0),Err(Error::BAD_HANDLE)) {return Err(Error::IO);}
    if vos::channel::send(999,b"")!=Err(Error::BAD_HANDLE) {return Err(Error::IO);}
    if vos::channel::receive(999,&mut data)!=Err(Error::BAD_HANDLE) || data!=[0x5A;4] {return Err(Error::IO);}
    if vos::close(999)!=Err(Error::BAD_HANDLE) {return Err(Error::IO);}
    vos::debug_write(b"SDK: console/files denied without grants; C ABI reused\n")?;
    Ok(())
}
fn run()->i32 {if demonstrate().is_ok() {0} else {1}}
