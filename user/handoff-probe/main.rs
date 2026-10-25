#![no_std]
#![no_main]
use vos_apps::{self as apps,Completion as C};
use vos::Error as E;
vos::entry!(run);
unsafe extern "C" {fn vos_handoff_verify(op:u64,value:u64)->i64;}
fn check(ok:bool,name:&[u8]) {if !ok{let _=vos::debug_write(b"handoff-probe FAIL");let _=vos::debug_write(name);vos::exit(91)}}
fn write(s:&[u8]) {check(vos::console::write_all(s).is_ok(),b"console write");}
fn run()->i32 {
    let mode=unsafe{vos_handoff_verify(0,0)};
    check((0..=20).contains(&mode),b"mode");
    check(unsafe{vos_handoff_verify(1,0)}==-2,b"private test bounds");
    check(unsafe{vos::raw::vos_file_initial(0)}<0,b"no filesystem grant");
    let (id,expected)=match mode {
        0=>(0x1003,Ok(C::Exited(0))),
        1=>(0x1002,Ok(C::Exited(0))),
        2=>(0x2001,Ok(C::Exited(-7))),
        3=>(0x2002,Ok(C::Faulted(14))),
        4|7|8|16=>(0x2003,Ok(C::Cancelled(1))),
        5|15=>(0x1002,Ok(C::Cancelled(1))),
        6|14=>(0x1002,Err(E::INTERRUPTED)),
        9=>(0x1003,Ok(C::Exited(1))),
        10|12=>(0x1002,Err(E::NO_MEMORY)),
        11=>(0x1002,Err(E::AGAIN)),
        13=>(0x1002,Err(E::UNSUPPORTED)),
        18=>(0x1002,Err(E::ACCESS)),
        20=>(0x2004,Ok(C::Exited(0))),
        _=>(0x1002,Ok(C::Exited(0))),
    };
    if mode==17 {
        check(apps::run(0xdead)==Err(E::ACCESS),b"unknown application");
        check(apps::run(0x1001)==Err(E::ACCESS),b"shell not executable");
        check(unsafe{apps::raw::vos_app_run_foreground(0x1002,1)}==-2,b"flags remain zero");
        let mut cookie=0;let mut n=0;let mut hello=false;let mut rixie=false;
        while let Some(e)=apps::next(cookie).unwrap_or_else(|_|{vos::exit(92)}) {
            hello|=e.app_id==0x1002 && &e.name[..5]==b"hello";
            rixie|=e.app_id==0x1003 && &e.name[..5]==b"rixie";
            check(e.app_id!=0x1001,b"hidden shell");cookie=e.next_cookie;n+=1;check(n<=8,b"catalog bound");
        }
        check(n==6 && hello && rixie,b"whitelist names");
    }
    if mode==19 {check(apps::next(0).err()==Some(E::ACCESS),b"enumeration rights separate");}
    if mode==0 {write(b"handoff-probe: interactive Rixie follows\n");}
    let result=apps::run(id);
    if mode==8 {check(false,b"terminated parent returned");}
    check(result==expected,b"completion");
    if matches!(mode,7|14|15|16) {
        check(vos::console::write(b"must not appear")==Err(E::ACCESS),b"revoked parent restored");
    } else {
        write(b"handoff-probe: console available\n");
    }
    if mode==6 {check(apps::next(0).err()==Some(E::ACCESS),b"execution revoked");}
    if mode==0 {
        write(b"Parent> ");let mut line=[0u8;129];
        let n=vos::console::read_line(&mut line).unwrap_or_else(|_|{vos::exit(93)});
        check(&line[..n]==b"parent-ok",b"child/transition input leaked to parent");
        write(b"handoff-probe: parent input isolation PASS\n");
    }
    let _=vos::debug_write(b"handoff-probe: PASS");
    0
}
