extern crate vos_gui as gui;
#[test] fn abi_sizes_offsets() {
    assert_eq!(core::mem::size_of::<gui::Create>(),24);
    assert_eq!(core::mem::size_of::<gui::Upload>(),32);
    assert_eq!(core::mem::size_of::<gui::Event>(),16);
    assert_eq!(core::mem::offset_of!(gui::Upload,pixels),24);
    assert_eq!(core::mem::offset_of!(gui::Event,sequence),8);
}
#[test] fn limits_and_explicit_formats() {
    assert_eq!(gui::MAX_WIDTH as usize*gui::MAX_HEIGHT as usize*4,163840);
    assert_eq!(gui::UPLOAD_MAX,4096);
}
