#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
mod native;
mod protocol;
mod runtime;
use anyhow::{Context, Result, bail};
use std::{net::IpAddr, path::PathBuf};
fn main() {
    if let Err(e) = run() {
        eprintln!("AirMirrorLite: {e:#}");
        if !std::env::args().any(|a| {
            matches!(
                a.as_str(),
                "--hidden" | "--self-test" | "--ui-test" | "--codec-worker"
            )
        }) {
            native::show_error(&format!("{e:#}"));
        }
        std::process::exit(1);
    }
}
fn run() -> Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args == ["--codec-worker"] {
        return native::worker_main();
    }
    if args.iter().any(|s| s == "--help") {
        println!(
            "AirMirrorLite\n  --ui-test           silent native UI and GPU tests\n  --self-test         full media tests, including an audible test tone\n  --ui-test-dir PATH  with --ui-test or --self-test, render idle/control artwork diagnostics\n  --ui-preview        synthetic video preview; no discovery or network\n  --no-discovery      run without DNS-SD (local integration tests)\n  --fps N            request 30 or 60 fps (default: 60)\n  --latency-ms N     shared video/audio buffer, 60..250 ms (default: 80)\n  --resolution N     request 1080, 1440 or 2160 lines (default: highest native support)\n  --bind IP           bind a specific local IPv4 address (default: 0.0.0.0)\n  --port PORT         control port (default: 7000)\n  --seconds N         stop after N seconds (validation)\n  --data-dir PATH     receiver identity and bounded log (default: EXE/local)\n  --hidden            hide window during automated validation\n\nWindow: drag to move; bottom-right resize; 0 default size; C hide/show controls; M mute; Up/Down volume; F11 fullscreen; D disconnect; Escape leaves fullscreen/disconnects; close exits."
        );
        return Ok(());
    }
    let mut bind: IpAddr = "0.0.0.0".parse()?;
    let mut port = 7000;
    let mut resolution = None;
    let mut fps = 60;
    let mut playout_ms = protocol::clock::DEFAULT_PLAYOUT_MS;
    let mut seconds = None;
    let mut advertise = true;
    let mut hidden = false;
    let mut probe = false;
    let mut silent_probe = false;
    let mut preview = false;
    let mut ui_test_dir = None;
    let mut data = std::env::current_exe()?
        .parent()
        .context("executable directory")?
        .join("local");
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--bind" => {
                i += 1;
                bind = args.get(i).context("--bind requires IP")?.parse()?;
            }
            "--fps" => {
                i += 1;
                fps = args
                    .get(i)
                    .context("--fps requires 30 or 60")?
                    .parse::<u32>()?;
                anyhow::ensure!(matches!(fps, 30 | 60), "fps must be 30 or 60");
            }
            "--latency-ms" => {
                i += 1;
                playout_ms = args
                    .get(i)
                    .context("--latency-ms requires 60..250")?
                    .parse::<u32>()?;
                protocol::clock::Clock::new(playout_ms)?;
            }
            "--resolution" => {
                i += 1;
                let requested = args
                    .get(i)
                    .context("--resolution requires 1080, 1440 or 2160")?
                    .parse::<u32>()?;
                anyhow::ensure!(
                    matches!(requested, 1080 | 1440 | 2160),
                    "resolution must be 1080, 1440 or 2160"
                );
                resolution = Some(requested);
            }
            "--port" => {
                i += 1;
                port = args.get(i).context("--port requires number")?.parse()?;
            }
            "--seconds" => {
                i += 1;
                seconds = Some(
                    args.get(i)
                        .context("--seconds requires number")?
                        .parse::<u64>()?,
                );
            }
            "--data-dir" => {
                i += 1;
                data = PathBuf::from(args.get(i).context("--data-dir requires path")?);
            }
            "--no-discovery" => advertise = false,
            "--hidden" => hidden = true,
            "--self-test" => probe = true,
            "--ui-test" => {
                probe = true;
                silent_probe = true;
            }
            "--ui-preview" => preview = true,
            "--ui-test-dir" => {
                i += 1;
                ui_test_dir = Some(PathBuf::from(
                    args.get(i).context("--ui-test-dir requires a path")?,
                ));
            }
            _ => bail!("unknown option {} (use --help)", args[i]),
        }
        i += 1;
    }
    anyhow::ensure!(
        ui_test_dir.is_none() || probe,
        "--ui-test-dir requires --ui-test or --self-test"
    );
    anyhow::ensure!(!(preview && probe), "choose --self-test or --ui-preview");
    anyhow::ensure!(bind.is_ipv4(), "--bind requires a local IPv4 address");
    let startup = std::time::Instant::now();
    let app = native::App::open(hidden || probe).context("native video/audio startup")?;
    let native_ms = startup.elapsed().as_secs_f64() * 1000.0;
    if preview {
        app.title("AirMirrorLite — synthetic UI preview (no network)");
        app.video_probe()?;
        let deadline = native::now() + seconds.unwrap_or(600) as f64;
        while native::now() < deadline {
            match app.pump(100)? {
                0 => break,
                2 => app.reset()?,
                _ => {}
            }
        }
        println!("Synthetic UI preview complete: {}", app.stats());
        return Ok(());
    }
    if !silent_probe {
        let mut worker = native::Worker::start().context("isolated native codec startup")?;
        anyhow::ensure!(worker.call(4, &[])? == [1], "worker probe");
        worker.configure(8)?;
        worker.configure(4)?;
    }
    if probe {
        if let Some(path) = &ui_test_dir {
            std::fs::create_dir_all(path)?;
        }
        app.ui_probe(
            &ui_test_dir
                .as_ref()
                .map(|p| p.to_string_lossy())
                .unwrap_or_default(),
        )?;
        app.video_probe()?;
        if silent_probe {
            native::codec_probe_silent()?;
            println!(
                "PASS: silent native UI, synthetic hardware video and isolated codec/IPC regression. No audio samples submitted.\n{}\nNo physical iPhone, sound or desktop-composition acceptance claimed.",
                app.stats()
            );
        } else {
            native::codec_probe(&app)?;
            println!(
                "PASS: native frameless UI geometry, hover volume/mute/reset/fullscreen/close controls; synthetic landscape/portrait H.264 decoded into D3D11 decoder surfaces and presented; known-answer FairPlay; AAC-ELD decoded in zero-capability AppContainer and sent to WASAPI.\n{}\nSynthetic media evidence only; iPhone and sustained A/V acceptance remain separate.",
                app.stats()
            );
        }
        return Ok(());
    }
    let profile = protocol::VideoProfile::new(
        resolution.unwrap_or(if app.hevc_available() { 2160 } else { 1080 }),
        app.hevc_available(),
    )?
    .with_fps(fps)?;
    let mut receiver = runtime::ReceiverApp::new(
        &data,
        bind,
        port,
        advertise,
        app.waker()?,
        profile,
        playout_ms,
    )?;
    receiver.shared.log(&format!(
        "startup native_ms={native_ms:.1} listener_ready_ms={:.1}",
        startup.elapsed().as_secs_f64() * 1000.0
    ));
    println!(
        "AirMirrorLite ready. Select it in iPhone Control Center > Screen Mirroring.\nLog: {}",
        data.join("receiver.log").display()
    );
    receiver.run(&app, seconds)
}
