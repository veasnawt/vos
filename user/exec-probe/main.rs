#![no_std]
#![no_main]
use vos_apps::{self as apps,Completion as C};
use vos::Error as E;
vos::entry!(run);
unsafe extern "C" {fn vos_exec_verify(mode:u64,value:u64)->i64;}
fn say(s:&[u8]) {if vos::debug_write(s).is_err(){vos::exit(98)}}
fn check(ok:bool,name:&[u8]) {if !ok {say(b"exec-probe FAIL");say(name);vos::exit(90)}}
fn control(mode:u64) {check(unsafe{vos_exec_verify(mode,0)}==0,b"control");}
fn passed(name:&[u8]) {say(name);}
fn raw_run(id:u64,flags:u64)->i64 {unsafe{apps::raw::vos_app_run_foreground(id,flags)}}
fn run()->i32 {
    check(vos::console::write(b"must not appear")==Err(E::ACCESS),b"console not granted");
    let first=apps::next(0).unwrap_or_else(|_|{vos::exit(91)}).unwrap_or_else(||{vos::exit(92)});
    check(first.app_id==0x1002 && &first.name[..5]==b"hello",b"filtered first identity");
    let old_cookie=first.next_cookie;
    let mut count=1;let mut cookie=old_cookie;
    while let Some(e)=apps::next(cookie).unwrap_or_else(|_|{vos::exit(93)}) {
        check(e.app_id!=0x1001 && e.app_id!=0x1003,b"hidden console applications");
        cookie=e.next_cookie;count+=1;check(count<=8,b"enumeration bound");
    }
    check(count==5,b"whitelist count");
    let mut out=apps::AppEntry::ZERO;
    check(unsafe{apps::raw::vos_app_next(0,&mut out,47)}==-2,b"short capacity");
    check(unsafe{apps::raw::vos_app_next(0,&mut out,49)}==-2,b"long capacity");
    check(unsafe{apps::raw::vos_app_next(0,1 as *mut apps::AppEntry,48)}==-3,b"invalid pointer");
    check(unsafe{apps::raw::vos_app_next((old_cookie&!15)|15,&mut out,48)}==-2,b"invalid cookie");
    check(raw_run(0,0)==-2 && raw_run(0x1002,1)==-2,b"run argument bounds");
    check(raw_run(0xdead,0)==-9 && raw_run(0x1001,0)==-9 && raw_run(0x1003,0)==-9,b"unauthorized identities");
    check(unsafe{vos_exec_verify(99,0)}==-2,b"invalid test mode");
    passed(b"enumeration and authority bounds PASS");
    check(apps::run(0x1002)==Ok(C::Exited(0)),b"hello execution");
    check(apps::run(0x2001)==Ok(C::Exited(-7)),b"negative exit width");
    check(apps::run(0x2002)==Ok(C::Faulted(14)),b"isolated child fault");
    check(apps::run(0x2004)==Ok(C::Exited(0)),b"recursive denied");
    passed(b"normal negative fault recursive PASS");
    control(1);check(apps::next(0).is_ok() && apps::run(0x1002)==Err(E::ACCESS),b"enumeration-only");
    control(2);check(apps::next(0).err()==Some(E::ACCESS) && apps::run(0x1002)==Ok(C::Exited(0)),b"execution-only");
    control(3);check(apps::next(0).err()==Some(E::ACCESS) && raw_run(0x1002,0)==-9,b"revoked grant");
    control(0);check(apps::next(old_cookie).err()==Some(E::STALE),b"stale generation");
    passed(b"rights revocation stale cookie PASS");
    for mode in [4,5,18] {
        control(mode);check(apps::run(0x1002)==Err(E::INTERRUPTED),b"pre-start cancellation");
        control(0);
    }
    passed(b"queued loaded pre-start cancellation PASS");
    for (mode,error) in [(11,E::NO_MEMORY),(12,E::NO_MEMORY),(13,E::AGAIN),(14,E::NO_MEMORY),(15,E::UNSUPPORTED)] {
        control(mode);check(apps::run(0x1002)==Err(error),b"rollback failure");
        control(0);
    }
    passed(b"allocation process thread loader rollback PASS");
    for mode in [6,7,16] {
        control(mode);check(apps::run(0x2003)==Ok(C::Cancelled(1)),b"running cancellation");
        control(0);
    }
    for mode in [8,9] {
        control(mode);check(apps::run(0x1002)==Ok(C::Cancelled(1)),b"reap publication cancellation");
        control(0);
    }
    control(10);check(apps::run(0x1002)==Ok(C::Exited(0)),b"immutable DONE");
    check(apps::next(0).err()==Some(E::ACCESS),b"post-completion revocation");
    control(0);
    passed(b"running reap publication contention PASS");
    for _ in 0..3 {check(apps::run(0x1002)==Ok(C::Exited(0)),b"fresh request");}
    passed(b"repeat execution PASS; final parent cancellation follows");
    control(17);
    let _=apps::run(0x2003);
    check(false,b"killed parent resumed");
    99
}
