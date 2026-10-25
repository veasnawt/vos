#![no_std]
#![no_main]

use vos::raw::*;
use vos_apps::{self as apps,Completion};
mod prompt;
use prompt::resolve;
vos::entry!(shell_main);
fn write(s: &[u8]) {if vos::console::write_all(s).is_err() {vos::exit(98)}}
fn error() { write(b"vsh: operation failed\n"); }
fn trim(s: &[u8]) -> &[u8] {
    let mut a=0; let mut b=s.len();
    while a<b && (s[a]==b' ' || s[a]==b'\t') { a+=1; }
    while b>a && (s[b-1]==b' ' || s[b-1]==b'\t') { b-=1; }
    &s[a..b]
}
fn open(base:isize,path:&[u8],rights:u64)->isize {
    if path.is_empty() { return unsafe { vos_file_open_at(base,b".".as_ptr(),1,rights) }; }
    unsafe { vos_file_open_at(base,path.as_ptr(),path.len(),rights) }
}
fn app_named(name:&[u8])->vos::Result<u64> {
    let mut cookie=0;
    for _ in 0..8 {
        match apps::next(cookie)? {
            None=>return Err(vos::Error::NO_ENTRY),
            Some(entry)=>{
                if &entry.name[..entry.name_length as usize]==name {return Ok(entry.app_id);}
                cookie=entry.next_cookie;
            }
        }
    }
    Err(vos::Error::IO)
}
fn number(value:i64) {
    let mut bytes=[0u8;21];let mut i=bytes.len();let mut n=value.unsigned_abs();
    loop {i-=1;bytes[i]=b'0'+(n%10) as u8;n/=10;if n==0{break;}}
    if value<0 {i-=1;bytes[i]=b'-';}
    write(&bytes[i..]);
}
fn launch(name:&[u8],arg:&[u8]) {
    if !arg.is_empty(){write(b"vsh: application arguments are not supported\n");return;}
    let id=match app_named(name){Ok(id)=>id,Err(_)=>{write(b"vsh: application unavailable or denied\n");return;}};
    match apps::run(id) {
        Ok(Completion::Exited(0))=>{},
        Ok(Completion::Exited(n))=>{write(b"vsh: application exited status ");number(n as i64);write(b"\n");},
        Ok(Completion::Faulted(n))=>{write(b"vsh: application fault vector ");number(n as i64);write(b"\n");},
        Ok(Completion::Cancelled(n))=>{write(b"vsh: application cancelled reason ");number(n as i64);write(b"\n");},
        Err(vos::Error::ACCESS)=>write(b"vsh: launch denied\n"),
        Err(_)=>write(b"vsh: application failed before start\n"),
    }
}
fn applications() {
    let mut cookie=0;
    for _ in 0..8 {
        match apps::next(cookie) {
            Ok(Some(entry))=>{write(&entry.name[..entry.name_length as usize]);write(b"\n");cookie=entry.next_cookie;},
            Ok(None)=>return,
            Err(_)=>{write(b"vsh: application catalog unavailable or denied\n");return;},
        }
    }
    write(b"vsh: invalid application catalog\n");
}
fn shell_main() -> i32 {
    write(b"VOS native vsh (Rust, ring 3)\nType help for commands.\n\n");
    let root=unsafe { vos_file_initial(0) };
    let mut cwd=[0u8;256]; let mut cwd_len=0;
    loop {
        let mut display=[0u8;prompt::PROMPT_MAX];
        let n=prompt::format(&cwd[..cwd_len],&mut display);
        write(&display[..n]);
        let mut line=[0u8;129];
        let count=unsafe { vos_console_read(line.as_mut_ptr(),line.len()) };
        if count<0 { error(); continue; }
        let text=trim(&line[..count as usize]);
        if text.is_empty() { continue; }
        let split=text.iter().position(|c| *c==b' ' || *c==b'\t').unwrap_or(text.len());
        let cmd=&text[..split]; let arg=trim(&text[split..]);
        match cmd {
            b"help" => write(b"Available commands: help, clear, echo, pwd, ls, cd, cat, apps, hello, rixie\n"),
            b"apps" => {if arg.is_empty(){applications();}else{error();}},
            b"hello" | b"rixie" => launch(cmd,arg),
            b"clear" => { if unsafe { vos_console_clear() }<0 { error(); } },
            b"echo" => { write(arg); write(b"\n"); },
            b"pwd" => {
                if root<0 {write(b"vsh: filesystem unavailable (no grant)\n");}
                else {let mut path=[0u8;257];path[0]=b'/';path[1..cwd_len+1].copy_from_slice(&cwd[..cwd_len]);write(&path[..cwd_len+1]);write(b"\n");}
            },
            b"exit" => { if root>0 { unsafe { vos_handle_close(root); } } return 0; },
            b"ls" | b"cd" | b"cat" => {
                if root<0 { write(b"vsh: filesystem unavailable (no grant)\n");continue; }
                let mut path=[0u8;256];
                let len=match resolve(&cwd[..cwd_len],arg,&mut path) {Some(n)=>n,None=>{error();continue;}};
                let rights=if cmd==b"cat" {3} else {14};
                let handle=open(root,&path[..len],rights);
                if handle<0 {error();continue;}
                if cmd==b"cd" { cwd[..len].copy_from_slice(&path[..len]);cwd_len=len; }
                else if cmd==b"ls" {
                    let mut cookie=0;
                    for _ in 0..512 {
                        let mut entry=[0u8;272];
                        let n=unsafe{vos_directory_next(handle,cookie,entry.as_mut_ptr(),entry.len())};
                        if n==0 {break;}
                        if n<0 {error();break;}
                        cookie=u64::from_le_bytes(entry[..8].try_into().unwrap());
                        let len=u32::from_le_bytes(entry[12..16].try_into().unwrap()) as usize;
                        if len>256 {error();break;}
                        write(&entry[16..16+len]);write(b"\n");
                    }
                } else {
                    let mut offset=0;
                    loop {
                        let mut data=[0u8;256];
                        let n=unsafe{vos_file_read_at(handle,data.as_mut_ptr(),data.len(),offset)};
                        if n==0 {break;}
                        if n<0 {error();break;}
                        write(&data[..n as usize]);offset+=n as u64;
                    }
                    write(b"\n");
                }
                unsafe { vos_handle_close(handle); }
            },
            _ => write(b"vsh: unknown command\n"),
        }
    }
}
