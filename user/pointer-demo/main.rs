//! Independent native ring-3 pointer exercise, not a window manager or Terminal.
#![no_std]
#![no_main]
use vos_gui as gui;
vos::entry!(run);
fn log(s:&[u8]) {let _=vos::debug_write(s);}
fn coordinate_record(x:u32,y:u32,down:bool,inside:bool)->Option<([u8;64],usize)> {
    fn append(out:&mut [u8;64],used:&mut usize,s:&[u8])->Option<()> {
        let end=used.checked_add(s.len())?;
        if end>out.len(){return None}
        out[*used..end].copy_from_slice(s);*used=end;Some(())
    }
    fn number(out:&mut [u8;64],used:&mut usize,mut n:u32)->Option<()> {
        let mut digits=[0u8;10];let mut i=digits.len();
        loop{i-=1;digits[i]=b'0'+(n%10) as u8;n/=10;if n==0{break}}
        append(out,used,&digits[i..])
    }
    let mut out=[0u8;64];let mut used=0;
    append(&mut out,&mut used,b"pointer-demo: x=")?;number(&mut out,&mut used,x)?;
    append(&mut out,&mut used,b" y=")?;number(&mut out,&mut used,y)?;
    append(&mut out,&mut used,if down{b" down=1"}else{b" down=0"})?;
    append(&mut out,&mut used,if inside{b" inside=1\n"}else{b" inside=0\n"})?;
    Some((out,used))
}
fn position(e:gui::Event) {
    if let Some((line,len))=coordinate_record(e.x(),e.y(),e.left(),e.inside()){
        log(&line[..len]);
    }
}
fn paint(s:&gui::Surface,down:bool,palette:bool)->vos::Result<()> {
    let mut row=[0u8;1024];
    for y in 0..160u32 {
        for x in 0..256u32 {
            let color:u32=if x<3 || y<3 || x>=253 || y>=157 {0x80baff}
                else if (40..216).contains(&x) && (40..120).contains(&y) {
                    if down {0xe8b34e}else if palette {0x42c497}else{0x409ddd}
                } else {0x101e30};
            row[x as usize*4..x as usize*4+4].copy_from_slice(&color.to_le_bytes());
        }
        s.upload(0,y,256,1,1024,&row)?;
    }
    s.present()
}
fn run()->i32 {
    let s=match gui::Surface::create(256,160){Ok(v)=>v,Err(_)=>return 1};
    if paint(&s,false,false).is_err(){return 2}
    log(b"pointer-demo: READY; move mouse; left click changes panel; a/b palette; q exits\n");
    let mut palette=false;let mut down=false;let mut barrier=0;
    loop {
        let e=match gui::pointer_event(true){Ok(v)=>v,Err(_)=>return 3};
        if e.kind==2 {
            let state=match gui::pointer_state(){Ok(v)=>v,Err(_)=>return 4};
            barrier=state.sequence;down=state.left();
            if paint(&s,down,palette).is_err(){return 5}
            log(b"pointer-demo: OVERFLOW snapshot recovered\n");continue
        }
        if e.sequence<=barrier {continue}
        if e.kind==1 {
            match e.code {
                113=>break,
                97|98=>{palette=e.code==97;if paint(&s,down,palette).is_err(){return 6}
                    log(if palette{b"pointer-demo: KEY a\n"}else{b"pointer-demo: KEY b\n"});},
                _=>{},
            }
        } else {
            if e.kind==gui::POINTER_BUTTON {
                down=e.left();if !down && e.inside() && (40..216).contains(&e.x()) && (40..120).contains(&e.y()){palette=!palette}
                if paint(&s,down,palette).is_err(){return 7}
                log(if down{b"pointer-demo: LEFT DOWN\n"}else{b"pointer-demo: LEFT UP\n"});
            }
            position(e);
        }
    }
    if s.release().is_err(){return 8}
    log(b"pointer-demo: RELEASED\n");0
}
