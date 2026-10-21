use std::ffi::CString;
unsafe extern "C"{fn generate(path:*const std::ffi::c_char)->i32;}
fn main(){let path=CString::new(std::env::args().nth(1).expect("output path")).unwrap();let result=unsafe{generate(path.as_ptr())};assert_eq!(result,0,"fixture generation failed");}
