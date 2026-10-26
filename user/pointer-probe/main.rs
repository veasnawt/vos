#![no_std]
#![no_main]
use vos_gui::{self as gui,raw::*};
vos::entry!(run);
unsafe extern "C" {fn vos_gui_verify(op:u64,zero:u64,zero2:u64)->i64;}
fn control(op:u64)->i64 {unsafe{vos_gui_verify(op,0,0)}}
fn check(ok:bool) {if !ok {let _=vos::debug_write(b"pointer-probe: FAIL\n");vos::exit(92)}}
fn run()->i32 {
    let mode=control(0);check((10..=14).contains(&mode));
    if mode==10 {check(gui::pointer_state()==Err(vos::Error::ACCESS));return 0}
    let _s=match gui::Surface::create(256,160){Ok(s)=>s,Err(_)=>{check(false);return 1}};
    let e=gui::pointer_state();check(e.map(|e|e.kind==5 && e.inside() && !e.left())==Ok(true));
    if mode==11 {
        check(unsafe{vos_gui_pointer_state(1 as *mut gui::Event,16,0)}==-3);
        let mut e=gui::Event::default();
        check(unsafe{vos_gui_pointer_state(&mut e,15,0)}==-2);
        check(unsafe{vos_gui_pointer_state(&mut e,16,1)}==-2);return 0
    }
    if mode==12 {
        let _=vos::debug_write(b"pointer-probe: OVERFLOW WINDOW\n");
        check(control(2)==0);
        check(gui::pointer_event(false).map(|e|e.kind==2 && e.code>0)==Ok(true));
        let snapshot=gui::pointer_state();check(snapshot.map(|e|!e.left() && e.sequence>16)==Ok(true));
        let _=vos::debug_write(b"pointer-probe: overflow snapshot PASS\n");return 0
    }
    check(control(1)==0);
    let r=gui::pointer_event(true);
    check(mode==14 && r==Err(vos::Error::INTERRUPTED));0
}
