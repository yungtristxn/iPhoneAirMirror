use std::ffi::CString;
unsafe extern "C" {
    fn generate(path: *const std::ffi::c_char, codec: u32) -> i32;
}
fn main() {
    let path = CString::new(std::env::args().nth(1).expect("output path")).unwrap();
    let codec = match std::env::args().nth(2).as_deref() {
        None | Some("eld") => 8,
        Some("lc") => 4,
        _ => panic!("codec must be eld or lc"),
    };
    let result = unsafe { generate(path.as_ptr(), codec) };
    assert_eq!(result, 0, "fixture generation failed");
}
