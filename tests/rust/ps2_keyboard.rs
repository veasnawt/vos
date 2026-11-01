use ps2_keyboard_core::Decoder;
fn feed(d:&mut Decoder, bytes:&[u8])->u8{let mut result=0;for &b in bytes{result=d.decode(b);}result}
#[test] fn independent_shift(){let mut d=Decoder::default();feed(&mut d,&[0x2a,0x36,0xaa]);assert_eq!(d.decode(0x1e),b'A');assert_eq!(d.shift,2);d.decode(0xb6);assert_eq!(d.decode(0x1e),b'a');}
#[test] fn caps_repeat(){let mut d=Decoder::default();feed(&mut d,&[0x3a,0x3a]);assert_eq!(d.caps,1);assert_eq!(d.decode(0x1e),b'A');feed(&mut d,&[0xba,0x3a]);assert_eq!(d.caps,0);}
#[test] fn caps_xor(){let mut d=Decoder::default();feed(&mut d,&[0x3a,0xba,0x2a]);assert_eq!(d.decode(0x1e),b'a');assert_eq!(d.decode(0x02),b'!');}
#[test] fn fake_shift(){let mut d=Decoder::default();feed(&mut d,&[0xe0,0x2a]);assert_eq!(d.shift,0);assert_eq!(d.decode(0x1e),b'a');}
#[test] fn pause(){let mut d=Decoder::default();d.decode(0xe1);assert_eq!(d.pause_left,5);for (i,b) in [0x1d,0x45,0xe1,0x9d,0xc5].into_iter().enumerate(){assert_eq!(d.decode(b),0);assert_eq!(usize::from(d.pause_left),4-i);}assert_eq!(d.ctrl,0);assert_eq!(d.decode(0x1e),b'a');}
#[test] fn ctrl(){let mut d=Decoder::default();d.decode(0x1d);assert_eq!(d.decode(0x1e),0);feed(&mut d,&[0xe0,0x1d,0x9d]);assert_eq!(d.decode(0x1e),0);feed(&mut d,&[0xe0,0x9d]);assert_eq!(d.decode(0x1e),b'a');}
#[test] fn alt(){let mut d=Decoder::default();d.decode(0x38);assert_eq!(d.decode(0x1e),0);feed(&mut d,&[0xe0,0x38,0xb8]);assert_eq!(d.decode(0x1e),0);feed(&mut d,&[0xe0,0xb8]);assert_eq!(d.decode(0x1e),b'a');}
#[test] fn breaks(){let mut d=Decoder::default();assert_eq!(d.decode(0x9e),0);assert_eq!(d.decode(0x1c),b'\n');assert_eq!(d.decode(0x0e),8);assert_eq!(d.decode(1),0);assert_eq!(d.decode(0x0f),0);}
#[test] fn punctuation(){let mut d=Decoder::default();d.decode(0x2a);assert_eq!(d.decode(0x02),b'!');assert_eq!(d.decode(0x28),b'"');}
#[test] fn noncanonical_bytes(){let mut d=Decoder{shift:1,caps:2,..Decoder::default()};assert_eq!(d.decode(0x1e),b'A');d.pause_left=255;assert_eq!(d.decode(0x1e),0);assert_eq!(d.pause_left,254);}
#[test] fn layout(){assert_eq!(core::mem::size_of::<Decoder>(),7);assert_eq!(core::mem::align_of::<Decoder>(),1);}
