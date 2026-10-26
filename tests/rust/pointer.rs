use vos_gui::Event;
#[test] fn layout_and_pointer_bits() {
    assert_eq!(core::mem::size_of::<Event>(),16);
    let e=Event{kind:4,code:255|(159<<8)|0x30000,sequence:u64::MAX};
    assert_eq!((e.x(),e.y(),e.left(),e.inside()),(255,159,true,true));
    let e=Event{kind:5,code:0,sequence:0};
    assert_eq!((e.x(),e.y(),e.left(),e.inside()),(0,0,false,false));
}
