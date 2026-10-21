use std::{fs, path::Path};

fn sources(build: &mut cc::Build, dir: &str, extension: &str) {
    let mut files: Vec<_> = fs::read_dir(dir)
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.extension().is_some_and(|s| s == extension))
        .collect();
    files.sort();
    build.files(files);
}

fn main() {
    assert_eq!(
        std::env::var("CARGO_CFG_TARGET_OS").as_deref(),
        Ok("windows"),
        "AirMirrorLite requires Windows"
    );
    println!("cargo:rerun-if-changed=native");
    println!("cargo:rerun-if-changed=vendor");
    let mut fairplay = cc::Build::new();
    fairplay
        .warnings(false)
        .include("vendor/playfair")
        .opt_level(2);
    sources(&mut fairplay, "vendor/playfair", "c");
    fairplay.compile("playfair");
    let parts = [
        "libAACdec",
        "libSBRdec",
        "libMpegTPDec",
        "libPCMutils",
        "libFDK",
        "libSYS",
        "libArithCoding",
        "libDRCdec",
        "libSACdec",
    ];
    let mut aac = cc::Build::new();
    aac.cpp(true)
        .warnings(false)
        .opt_level(2)
        .define("FDK_AAC_STATIC", None)
        .define("NOMINMAX", None);
    for part in parts {
        let dir = format!("vendor/fdk-aac/{part}");
        if Path::new(&dir).exists() {
            aac.include(format!("{dir}/include"));
            sources(&mut aac, &format!("{dir}/src"), "cpp");
        }
    }
    aac.file("native/codec.cpp").compile("aac_decoder");
    cc::Build::new()
        .cpp(true)
        .std("c++17")
        .flag("/EHsc")
        .flag("/utf-8")
        .flag("/W4")
        .define("UNICODE", None)
        .define("_UNICODE", None)
        .define("NOMINMAX", None)
        .file("native/windows.cpp")
        .file("native/phone_shell.cpp")
        .compile("windows_bridge");
    for lib in [
        "mfplat",
        "mfuuid",
        "mf",
        "wmcodecdspuuid",
        "d3d11",
        "dxgi",
        "dcomp",
        "d2d1",
        "dwrite",
        "windowscodecs",
        "gdiplus",
        "dwmapi",
        "comctl32",
        "ole32",
        "oleaut32",
        "user32",
        "gdi32",
        "dnsapi",
        "iphlpapi",
        "ws2_32",
        "advapi32",
        "userenv",
        "bcrypt",
        "crypt32",
        "propsys",
        "avrt",
    ] {
        println!("cargo:rustc-link-lib={lib}");
    }
}
