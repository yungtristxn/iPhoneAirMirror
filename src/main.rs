#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
mod native;
mod protocol;
mod runtime;
use anyhow::{Context, Result, bail};
use std::{net::IpAddr, path::PathBuf};
fn main() {
    if let Err(e) = run() {
        eprintln!("AirMirrorLite: {e:#}");
        if !std::env::args()
            .any(|a| matches!(a.as_str(), "--hidden" | "--self-test" | "--codec-worker"))
        {
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
            "AirMirrorLite\n  --self-test         test Windows media initialization and isolated codec worker\n  --no-discovery      run without DNS-SD (local integration tests)\n  --bind IP           bind a specific local IPv4 address (default: 0.0.0.0)\n  --port PORT         control port (default: 7000)\n  --seconds N         stop after N seconds (validation)\n  --data-dir PATH     receiver identity and bounded log (default: EXE/local)\n  --hidden            hide window during automated validation\n\nWindow: F11 fullscreen; D disconnect; Escape leaves fullscreen/disconnects; close exits."
        );
        return Ok(());
    }
    let mut bind: IpAddr = "0.0.0.0".parse()?;
    let mut port = 7000;
    let mut seconds = None;
    let mut advertise = true;
    let mut hidden = false;
    let mut probe = false;
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
            _ => bail!("unknown option {} (use --help)", args[i]),
        }
        i += 1;
    }
    let app = native::App::open(hidden || probe).context("native video/audio startup")?;
    {
        let mut worker = native::Worker::start().context("isolated native codec startup")?;
        anyhow::ensure!(worker.call(4, &[])? == [1], "worker probe");
        worker.configure(8)?;
        worker.configure(4)?;
    }
    if probe {
        app.video_probe()?;
        native::codec_probe(&app)?;
        println!(
            "PASS: synthetic landscape/portrait H.264 decoded into D3D11 decoder surfaces and presented; known-answer FairPlay; AAC-ELD decoded in zero-capability AppContainer and sent to WASAPI.\n{}\nSynthetic media evidence only; iPhone and sustained A/V acceptance remain separate.",
            app.stats()
        );
        return Ok(());
    }
    let mut receiver = runtime::ReceiverApp::new(&data, bind, port, advertise, app.waker()?)?;
    println!(
        "AirMirrorLite ready. Select it in iPhone Control Center > Screen Mirroring.\nLog: {}",
        data.join("receiver.log").display()
    );
    receiver.run(&app, seconds)
}
