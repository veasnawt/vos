#![no_std]
#![no_main]
use vos_gui::{self as gui,raw::*};
vos::entry!(run);
unsafe extern "C" {fn vos_gui_verify(op:u64,zero:u64,zero2:u64)->i64;}
fn control(op:u64)->i64 {unsafe{vos_gui_verify(op,0,0)}}
fn check(ok:bool) {if !ok {let _=vos::debug_write(b"gui-probe: FAIL\n");vos::exit(91)}}
fn descriptor(w:u32,h:u32)->gui::Create {gui::Create{version:1,size:24,width:w,height:h,stride:w.wrapping_mul(4),format:1}}
fn create(d:&gui::Create)->i64 {unsafe{vos_gui_create(d,24)}}
fn present(token:u64)->i64 {unsafe{vos_gui_present(token,0,0)}}
fn run()->i32 {
    let mode=control(0);check((0..=9).contains(&mode));
    let _=vos::debug_write(b"gui-probe: actual ABI5 ring-3 invocation\n");
    if mode==3 {
        check(create(&descriptor(1,1))==-9);
        check(present(1)==-9);
        check(gui::event(false)==Err(vos::Error::ACCESS));return 0
    }
    if mode==1 {
        for (w,h) in [(0,1),(1,0),(257,1),(1,161),(u32::MAX,1),(1,u32::MAX)] {
            check(create(&descriptor(w,h))==-2);
        }
        check(unsafe{vos_gui_create(1 as *const gui::Create,24)}==-3);
        let mut d=descriptor(1,1);d.version=2;check(create(&d)==-2);d.version=1;d.stride=3;check(create(&d)==-2);
        return 0
    }
    if mode==4 {check(create(&descriptor(256,160))==-7);return 0}
    if mode==9 {check(gui::event(false)==Err(vos::Error::AGAIN));return 0}
    let token=create(&descriptor(256,160));check(token>0);let token=token as u64;
    if mode==5 {unsafe{core::arch::asm!("ud2",options(noreturn))}}
    if mode==6 || mode==7 {
        check(control(1)==0);
        let r=gui::event(true);
        check(mode==7 && r==Err(vos::Error::INTERRUPTED));return 0
    }
    if mode==8 {
        let _=vos::debug_write(b"gui-probe: OVERFLOW INPUT WINDOW\n");
        check(control(2)==0);
        let event=gui::event(false);check(event.map(|e|e.kind==2 && e.code>0)==Ok(true));
        let _=vos::debug_write(b"gui-probe: native overflow reported\n");return 0
    }
    let pixels=[0x55u8;16];
    let mut d=gui::Upload{x:0,y:0,width:2,height:2,stride:8,length:16,pixels:pixels.as_ptr() as u64};
    check(unsafe{vos_gui_upload(token,&d,32)}==0);
    if mode==2 {
        d.x=u32::MAX;check(unsafe{vos_gui_upload(token,&d,32)}==-2);d.x=0;
        d.width=u32::MAX;check(unsafe{vos_gui_upload(token,&d,32)}==-2);d.width=2;
        d.stride=7;check(unsafe{vos_gui_upload(token,&d,32)}==-2);d.stride=8;
        d.length=15;check(unsafe{vos_gui_upload(token,&d,32)}==-2);d.length=16;
        d.pixels=1;check(unsafe{vos_gui_upload(token,&d,32)}==-3);
        check(present(token+1)==-6);
        check(unsafe{vos_gui_event(1 as *mut gui::Event,16,1)}==-3);
        check(unsafe{vos_gui_present(token,1,0)}==-2);
    }
    check(present(token)==0);
    if mode==0 {
        check(unsafe{vos_gui_release(token,0,0)}==0);
        check(present(token)==-6);
    }
    // Other cases intentionally retain their surface to exercise process-end reclamation.
    0
}
