#![no_std]
#![no_main]
use vos_gui as gui;
vos::entry!(run);
fn log(s:&[u8]) {let _=vos::debug_write(s);}
fn paint(surface:&gui::Surface,palette:u32)->vos::Result<()> {
    let mut row=[0u8;256*4];
    for y in 0..160u32 {
        for x in 0..256u32 {
            // Deliberately simple application-owned geometry; no branding/icons.
            let color=if x<3 || y<3 || x>=253 || y>=157 {0x80baff}
                else if (16..40).contains(&y) && (16..240).contains(&x) {if palette==0{0x409ddd}else{0x42c497}}
                else if (56..140).contains(&y) && (16..240).contains(&x) {
                    if ((x-16)/28+(y-56)/28)%2==0 {0x20456b+palette*0x102000}else{0x142e4a}
                } else {0x101e30};
            let at=x as usize*4;row[at..at+4].copy_from_slice(&u32::to_le_bytes(color));
        }
        surface.upload(0,y,256,1,1024,&row)?;
    }
    surface.present()
}
fn run()->i32 {
    let surface=match gui::Surface::create(256,160){Ok(s)=>s,Err(_)=>return 1};
    if paint(&surface,0).is_err(){return 2}
    log(b"gui-demo: PRESENTED; Rust ring 3; a/b changes pixels; q exits\n");
    loop {
        let event=match gui::event(true){Ok(e)=>e,Err(_)=>return 3};
        if event.kind==2 {log(b"gui-demo: explicit input overflow\n");continue}
        match event.code {
            97|98 => {
                if paint(&surface,if event.code==97{1}else{0}).is_err(){return 4}
                log(if event.code==97{b"gui-demo: KEY a PRESENTED\n"}else{b"gui-demo: KEY b PRESENTED\n"});
            },
            113 => break,
            _=>{},
        }
    }
    if surface.release().is_err(){return 5}
    log(b"gui-demo: RELEASED; clean exit\n");0
}
