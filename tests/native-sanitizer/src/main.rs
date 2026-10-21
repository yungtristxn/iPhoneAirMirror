fn main(){let fixtures=std::path::Path::new(env!("FIXTURES"));let status=std::process::Command::new(env!("NATIVE_SANITIZER"))
    .arg(fixtures.join("fairplay-known-answer.bin")).arg(fixtures.join("tone-44100-stereo-eld.frames")).status().unwrap();assert!(status.success(),"native ASan probe failed: {status}");}
