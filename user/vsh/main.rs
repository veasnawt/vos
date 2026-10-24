#![no_std]
#![no_main]

use core::panic::PanicInfo;
unsafe extern "C" {
    fn vos_console_read(p: *mut u8, cap: usize) -> isize;
    fn vos_console_write(p: *const u8, len: usize) -> isize;
    fn vos_console_clear() -> isize;
    fn vos_exit(status: i32) -> !;
    fn vos_file_initial(index: u64) -> isize;
    fn vos_file_open_at(base: isize, path: *const u8, len: usize, rights: u64) -> isize;
    fn vos_file_read_at(file: isize, out: *mut u8, len: usize, offset: u64) -> isize;
    fn vos_directory_next(base: isize, cookie: u64, out: *mut u8, len: usize) -> isize;
    fn vos_handle_close(handle: isize) -> isize;
}
#[panic_handler]
fn panic(_: &PanicInfo) -> ! { unsafe { vos_exit(99) } }
fn write(s: &[u8]) { for part in s.chunks(256) { if unsafe { vos_console_write(part.as_ptr(),part.len()) } < 0 { unsafe { vos_exit(98) } } } }
fn error() { write(b"vsh: operation failed\n"); }
fn trim(s: &[u8]) -> &[u8] {
    let mut a=0; let mut b=s.len();
    while a<b && (s[a]==b' ' || s[a]==b'\t') { a+=1; }
    while b>a && (s[b-1]==b' ' || s[b-1]==b'\t') { b-=1; }
    &s[a..b]
}
/* Lexically resolve within the explicitly granted root. Never submit '..' to
 * the file API and never permit cd above that root. Fixed storage, no heap. */
fn resolve(cwd: &[u8], arg: &[u8], out: &mut [u8;256]) -> Option<usize> {
    let mut n=0;
    if !arg.starts_with(b"/") { out[..cwd.len()].copy_from_slice(cwd); n=cwd.len(); }
    for p in arg.split(|c| *c==b'/') {
        if p.is_empty() || p==b"." { continue; }
        if p==b".." { while n>0 && out[n-1]!=b'/' { n-=1; } if n>0 { n-=1; } continue; }
        if n+p.len()+1>out.len() { return None; }
        if n>0 { out[n]=b'/'; n+=1; }
        out[n..n+p.len()].copy_from_slice(p); n+=p.len();
    }
    Some(n)
}
fn open(base:isize,path:&[u8],rights:u64)->isize {
    if path.is_empty() { return unsafe { vos_file_open_at(base,b".".as_ptr(),1,rights) }; }
    unsafe { vos_file_open_at(base,path.as_ptr(),path.len(),rights) }
}
#[unsafe(no_mangle)]
pub extern "C" fn main() -> i32 {
    write(b"VOS native vsh (Rust, ring 3)\nType help for commands.\n\n");
    let root=unsafe { vos_file_initial(0) };
    let mut cwd=[0u8;256]; let mut cwd_len=0;
    loop {
        write(b"VOS> ");
        let mut line=[0u8;129];
        let count=unsafe { vos_console_read(line.as_mut_ptr(),line.len()) };
        if count<0 { error(); continue; }
        let text=trim(&line[..count as usize]);
        if text.is_empty() { continue; }
        let split=text.iter().position(|c| *c==b' ' || *c==b'\t').unwrap_or(text.len());
        let cmd=&text[..split]; let arg=trim(&text[split..]);
        match cmd {
            b"help" => write(b"Available commands: help, clear, echo, pwd, ls, cd, cat\n"),
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
