use std::{fs, path::PathBuf};
fn main() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../work/fdk-aac-c41c3c7516ea8d0d78a53b96b30bed3a29745cc6/cpp");
    assert!(root.is_dir(),"fixture generation needs the pinned full FDK source under work/fdk-aac-c41c3c7516ea8d0d78a53b96b30bed3a29745cc6/cpp; the receiver intentionally vendors decoder sources only");
    println!("cargo:rerun-if-changed=generator.cpp");
    println!("cargo:rerun-if-changed={}", root.display());
    let mut b = cc::Build::new();
    b.cpp(true)
        .warnings(false)
        .opt_level(2)
        .define("FDK_AAC_STATIC", None);
    for part in [
        "libAACenc",
        "libSBRenc",
        "libSACenc",
        "libMpegTPEnc",
        "libPCMutils",
        "libFDK",
        "libSYS",
    ] {
        b.include(root.join(part).join("include"));
        let mut files: Vec<_> = fs::read_dir(root.join(part).join("src"))
            .unwrap()
            .map(|e| e.unwrap().path())
            .filter(|p| p.extension().is_some_and(|s| s == "cpp"))
            .collect();
        files.sort();
        b.files(files);
    }
    b.file("generator.cpp").compile("fixture_encoder");
}
