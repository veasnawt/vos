#![no_std]
#![no_main]

mod dialog;
vos::entry!(run);

fn session() -> vos::Result<()> {
    vos::console::write_all(dialog::BANNER)?;
    loop {
        vos::console::write_all(b"Rixie> ")?;
        let mut line=[0u8;129]; // Existing 128-byte line bound plus the NUL.
        let n=vos::console::read_line(&mut line)?;
        let reply=dialog::respond(&line[..n]);
        vos::console::write_all(reply.text)?;
        if reply.exit {return Ok(());}
    }
}

fn run() -> i32 {
    if session().is_ok() {0}
    else {let _=vos::debug_write(b"Rixie: console I/O failed.\n");1}
}
