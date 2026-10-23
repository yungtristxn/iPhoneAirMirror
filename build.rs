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

fn json_string(value: &str) -> String {
    let mut out = String::from("\"");
    for character in value.chars() {
        match character {
            '\\' => out.push_str("\\\\"),
            '"' => out.push_str("\\\""),
            c if c.is_control() => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

fn compiler_provenance(build: &cc::Build) -> String {
    let compiler = build.get_compiler();
    let flags = compiler
        .args()
        .iter()
        .map(|v| json_string(&v.to_string_lossy()))
        .collect::<Vec<_>>()
        .join(",");
    let environment = compiler
        .env()
        .iter()
        .filter(|(key, _)| matches!(key.to_str(), Some("INCLUDE" | "LIB")))
        .map(|(key, value)| {
            format!(
                "{}:{}",
                json_string(&key.to_string_lossy()),
                json_string(&value.to_string_lossy())
            )
        })
        .collect::<Vec<_>>()
        .join(",");
    format!(
        "{{\"path\":{},\"flags\":[{}],\"sdkEnvironment\":{{{}}}}}",
        json_string(&compiler.path().to_string_lossy()),
        flags,
        environment
    )
}

fn ffmpeg_codec() -> String {
    println!("cargo:rerun-if-env-changed=AIRMIRROR_FFMPEG_PREFIX");
    let prefix = std::env::var_os("AIRMIRROR_FFMPEG_PREFIX")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| Path::new("work/ffmpeg-aac-msvc").to_path_buf());
    let prefix = prefix
        .canonicalize()
        .expect("run scripts/Build-FFmpeg.ps1 before enabling ffmpeg-aac");
    // MSVC's include search does not accept Rust's extended-length path prefix.
    let prefix = std::path::PathBuf::from(prefix.to_string_lossy().trim_start_matches(r"\\?\"));
    println!(
        "cargo:rerun-if-changed={}",
        prefix.join("include").display()
    );
    println!("cargo:rerun-if-changed=scripts/Verify-FFmpeg.ps1");
    println!("cargo:rerun-if-changed=scripts/ffmpeg-pins.json");
    let verified = std::process::Command::new("pwsh.exe")
        .args([
            "-NoProfile",
            "-NonInteractive",
            "-File",
            "scripts/Verify-FFmpeg.ps1",
            "-Prefix",
        ])
        .arg(&prefix)
        .output()
        .expect("PowerShell 7 is required to verify the optional FFmpeg experiment build");
    assert!(
        verified.status.success(),
        "FFmpeg dependency verification failed: {}{}",
        String::from_utf8_lossy(&verified.stdout),
        String::from_utf8_lossy(&verified.stderr)
    );
    let manifest_path = prefix.join("airmirror-ffmpeg.json");
    let config_path = prefix.join("config/config.h");
    let components_path = prefix.join("config/config_components.h");
    for path in [&manifest_path, &config_path, &components_path] {
        println!("cargo:rerun-if-changed={}", path.display());
    }
    let manifest = fs::read_to_string(manifest_path).expect("pinned FFmpeg build provenance");
    let config = fs::read_to_string(config_path).expect("FFmpeg configuration");
    let components = fs::read_to_string(components_path).expect("FFmpeg components");
    let definitions: std::collections::BTreeMap<_, _> = config
        .lines()
        .chain(components.lines())
        .filter_map(|line| {
            let mut words = line.split_whitespace();
            (words.next()? == "#define").then_some((words.next()?, words.next()?))
        })
        .collect();
    assert_eq!(
        definitions.get("CONFIG_AAC_DECODER"),
        Some(&"1"),
        "native float AAC decoder required"
    );
    for (key, value) in &definitions {
        if *value != "1" {
            continue;
        }
        let forbidden = (key.ends_with("_DECODER") && *key != "CONFIG_AAC_DECODER")
            || key.ends_with("_ENCODER")
            || key.ends_with("_DEMUXER")
            || key.ends_with("_MUXER")
            || key.ends_with("_PROTOCOL")
            || key.ends_with("_FILTER")
            || key.ends_with("_HWACCEL")
            || key.ends_with("_PARSER")
            || key.ends_with("_BSF")
            || matches!(
                *key,
                "CONFIG_NETWORK"
                    | "CONFIG_NONFREE"
                    | "CONFIG_LIBFDK_AAC"
                    | "CONFIG_FFMPEG"
                    | "CONFIG_FFPLAY"
                    | "CONFIG_FFPROBE"
            );
        assert!(!forbidden, "unexpected FFmpeg capability: {key}");
    }
    println!(
        "cargo:rustc-link-search=native={}",
        prefix.join("lib").display()
    );
    for library in ["avcodec", "avutil"] {
        let file = prefix.join("lib").join(format!("{library}.lib"));
        assert!(
            file.is_file(),
            "missing static FFmpeg library {}",
            file.display()
        );
        println!("cargo:rerun-if-changed={}", file.display());
    }
    let mut codec = cc::Build::new();
    codec
        .cpp(true)
        .std("c++17")
        .flag("/EHsc")
        .flag("/W4")
        .define("NOMINMAX", None)
        .include(prefix.join("include"))
        .file("native/codec_ffmpeg.cpp")
        .compile("aac_decoder");
    println!("cargo:rustc-link-lib=static=avcodec");
    println!("cargo:rustc-link-lib=static=avutil");
    format!(
        "{{\"backend\":\"ffmpeg-native-aac\",\"bridge\":{},\"upstream\":{}}}",
        compiler_provenance(&codec),
        manifest.trim_start_matches('\u{feff}').trim()
    )
}

fn main() {
    assert_eq!(
        std::env::var("TARGET").as_deref(),
        Ok("x86_64-pc-windows-msvc"),
        "AirMirrorLite requires the x86_64-pc-windows-msvc target"
    );
    println!("cargo:rerun-if-changed=native");
    println!("cargo:rerun-if-changed=vendor/playfair");
    println!("cargo:rerun-if-changed=assets/app-icon.ico");
    // Use the installed SDK resource compiler; no new build/runtime dependency.
    let sdk = std::env::var_os("ProgramFiles(x86)").expect("Windows SDK location");
    let sdk_bin = Path::new(&sdk).join("Windows Kits/10/bin");
    let mut versions: Vec<_> = fs::read_dir(sdk_bin)
        .expect("Windows SDK bin")
        .filter_map(Result::ok)
        .map(|e| e.path().join("x64/rc.exe"))
        .filter(|p| p.is_file())
        .collect();
    versions.sort();
    let rc = versions.last().expect("Windows SDK rc.exe is required");
    let resource = Path::new(&std::env::var_os("OUT_DIR").unwrap()).join("app.res");
    let status = std::process::Command::new(rc)
        .current_dir("native")
        .arg("/nologo")
        .arg("/fo")
        .arg(&resource)
        .arg("app.rc")
        .status()
        .expect("compile application icon");
    assert!(status.success(), "application resource compilation failed");
    println!("cargo:rustc-link-arg={}", resource.display());
    let mut fairplay = cc::Build::new();
    fairplay
        .warnings(false)
        .include("vendor/playfair")
        .opt_level(2);
    sources(&mut fairplay, "vendor/playfair", "c");
    fairplay.compile("playfair");
    let aac_provenance = if std::env::var_os("CARGO_FEATURE_FFMPEG_AAC").is_some() {
        ffmpeg_codec()
    } else {
        println!("cargo:rerun-if-changed=vendor/fdk-aac");
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
        compiler_provenance(&aac)
    };
    let mut bridge = cc::Build::new();
    bridge
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
        .file("native/phone_accessibility.cpp")
        .compile("windows_bridge");
    let provenance = format!(
        "{{\"target\":\"x86_64-pc-windows-msvc\",\"resourceCompiler\":{},\"resourceFlags\":[\"/nologo\",\"/fo <OUT_DIR>/app.res\",\"app.rc\"],\"playfair\":{},\"aac\":{},\"bridge\":{}}}",
        json_string(&rc.to_string_lossy()),
        compiler_provenance(&fairplay),
        aac_provenance,
        compiler_provenance(&bridge)
    );
    fs::write(
        Path::new(&std::env::var_os("OUT_DIR").unwrap()).join("native-build.json"),
        provenance,
    )
    .expect("write native compiler provenance");
    for lib in [
        "mfplat",
        "mfuuid",
        "mf",
        "wmcodecdspuuid",
        "d3d11",
        "dxgi",
        "dxguid",
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
        "uiautomationcore",
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
