use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
};
fn files(b: &mut cc::Build, dir: &Path, ext: &str) {
    let mut p: Vec<_> = fs::read_dir(dir)
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.extension().is_some_and(|s| s == ext))
        .collect();
    p.sort();
    b.files(p);
}
fn main() {
    let canonical = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .canonicalize()
        .unwrap();
    let root = PathBuf::from(canonical.to_string_lossy().trim_start_matches(r"\\?\"));
    // These inputs live outside this Cargo package. Without explicit watches,
    // an edited codec/vendor source can silently run the previous ASan binary.
    for path in [
        root.join("native/codec.cpp"),
        root.join("vendor/playfair"),
        root.join("vendor/fdk-aac"),
        PathBuf::from("harness.cpp"),
    ] {
        println!("cargo:rerun-if-changed={}", path.display());
    }
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let mut fair = cc::Build::new();
    fair.cargo_metadata(false)
        .warnings(false)
        .opt_level(1)
        .flag("/fsanitize=address");
    files(&mut fair, &root.join("vendor/playfair"), "c");
    fair.compile("fair_san");
    let mut aac = cc::Build::new();
    aac.cargo_metadata(false)
        .cpp(true)
        .warnings(false)
        .opt_level(1)
        .flag("/fsanitize=address")
        .define("FDK_AAC_STATIC", None)
        .define("NOMINMAX", None);
    for part in [
        "libAACdec",
        "libSBRdec",
        "libMpegTPDec",
        "libPCMutils",
        "libFDK",
        "libSYS",
        "libArithCoding",
        "libDRCdec",
        "libSACdec",
    ] {
        let dir = root.join("vendor/fdk-aac").join(part);
        aac.include(dir.join("include"));
        files(&mut aac, &dir.join("src"), "cpp");
    }
    aac.file(root.join("native/codec.cpp")).compile("aac_san");
    let compiler = aac.get_compiler();
    let exe = out.join("native-sanitizer.exe");
    let mut command: Command = compiler.to_command();
    command
        .args([
            "/nologo",
            "/EHsc",
            "/O1",
            "/Zi",
            "/fsanitize=address",
            "harness.cpp",
        ])
        .arg(out.join("fair_san.lib"))
        .arg(out.join("aac_san.lib"))
        .arg(format!("/Fe:{}", exe.display()))
        .arg(format!("/Fo:{}", out.join("harness.obj").display()))
        .arg(format!("/Fd:{}", out.join("harness.pdb").display()))
        .args(["/link", "/INCREMENTAL:NO"]);
    assert!(
        command.status().unwrap().success(),
        "native sanitizer link failed"
    );
    let runtime = compiler
        .path()
        .parent()
        .unwrap()
        .join("clang_rt.asan_dynamic-x86_64.dll");
    fs::copy(runtime, out.join("clang_rt.asan_dynamic-x86_64.dll")).unwrap();
    println!("cargo:rustc-env=NATIVE_SANITIZER={}", exe.display());
    println!(
        "cargo:rustc-env=FIXTURES={}",
        root.join("tests/fixtures").display()
    );
}
