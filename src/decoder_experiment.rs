//! Standalone synthetic-fixture experiment. No App, WASAPI, display, discovery,
//! user media input or capture path is reachable from this module.
use crate::native;
use anyhow::{Context, Result, ensure};
use sha2::{Digest, Sha256};
use std::{
    fs::{self, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    time::Instant,
};

const RATE: usize = 44100;
const CHANNELS: usize = 2;
const PACKETS: usize = 120;
const WORKER_LIMIT: u64 = 256 * 1024 * 1024;
const SOURCE_START: u32 = 441000;

struct Fixture {
    name: &'static str,
    codec: u8,
    samples_per_frame: usize,
    bytes: &'static [u8],
    fdk_hash: Option<&'static str>,
    signal: &'static str,
}
const FIXTURES: [Fixture; 4] = [
    Fixture {
        name: "eld",
        codec: 8,
        samples_per_frame: 480,
        bytes: include_bytes!("../tests/fixtures/tone-44100-stereo-eld.frames"),
        fdk_hash: Some("c5679c95ef23189c71327d0e78e26ffd94c596f40902ccdfb7f5feaf256f8f6f"),
        signal: "periodic",
    },
    Fixture {
        name: "lc",
        codec: 4,
        samples_per_frame: 1024,
        bytes: include_bytes!("../tests/fixtures/triangle-44100-stereo-lc.frames"),
        fdk_hash: Some("ec0e0ecf994a43665ab2ef72c245ca49385d5927353a531e97dbe84707b59ae9"),
        signal: "periodic",
    },
    Fixture {
        name: "eld-experiment",
        codec: 8,
        samples_per_frame: 480,
        bytes: include_bytes!("../tests/fixtures/noise-44100-stereo-eld-experiment.frames"),
        fdk_hash: None,
        signal: "nonperiodic",
    },
    Fixture {
        name: "lc-experiment",
        codec: 4,
        samples_per_frame: 1024,
        bytes: include_bytes!("../tests/fixtures/noise-44100-stereo-lc-experiment.frames"),
        fdk_hash: None,
        signal: "nonperiodic",
    },
];
#[derive(Default)]
struct CodecReport {
    hash: String,
    hashes: Vec<String>,
    decode_ms: Vec<f64>,
    configure_ms: Vec<f64>,
    reported_delay_samples: Option<u32>,
    additional_delay_samples: Option<u32>,
    normalized_hashes: Vec<String>,
    pass_generations: Vec<u64>,
    pass_delay_samples: Vec<u32>,
    normalized_pcm: Vec<i16>,
    error: String,
}
struct SessionReport {
    index: usize,
    wall_ms: f64,
    worker_cpu_ms: Option<f64>,
    private_bytes: Vec<u64>,
    error: String,
}
struct GuardReport {
    codec: &'static str,
    name: &'static str,
    rejected: bool,
    bounded: bool,
    recovered: bool,
    elapsed_ms: f64,
    diagnostic: String,
}
struct Report {
    backend: String,
    sessions_requested: usize,
    codecs: [CodecReport; 4],
    sessions: Vec<SessionReport>,
    guards: Vec<GuardReport>,
    failure: String,
}
fn packets(bytes: &[u8]) -> Result<Vec<&[u8]>> {
    let mut result = Vec::with_capacity(PACKETS);
    let mut offset = 0usize;
    while offset < bytes.len() {
        ensure!(result.len() < PACKETS, "fixture packet-count limit");
        let length = bytes
            .get(offset..offset + 4)
            .context("truncated fixture length")?;
        let length = u32::from_le_bytes(length.try_into()?) as usize;
        ensure!((1..=8192).contains(&length), "fixture packet length");
        offset += 4;
        let end = offset
            .checked_add(length)
            .context("fixture length overflow")?;
        result.push(bytes.get(offset..end).context("truncated fixture packet")?);
        offset = end;
    }
    ensure!(
        result.len() == PACKETS,
        "fixture must contain exactly120 packets"
    );
    Ok(result)
}
fn milliseconds(start: Instant) -> f64 {
    start.elapsed().as_secs_f64() * 1000.0
}
fn brief(error: &anyhow::Error) -> String {
    format!("{error:#}").chars().take(1024).collect()
}
fn decoder_failure(worker: &mut native::Worker, error: anyhow::Error) -> anyhow::Error {
    let detail = worker.decoder_error().unwrap_or_default();
    if detail.is_empty() {
        error
    } else {
        error.context(detail)
    }
}
fn decode_fixture(
    worker: &mut native::Worker,
    fixture: &Fixture,
    timing: &mut CodecReport,
    private: &mut Vec<u64>,
    keep_pcm: bool,
) -> Result<(String, Vec<i16>)> {
    let began = Instant::now();
    worker
        .configure(fixture.codec)
        .map_err(|e| decoder_failure(worker, e))?;
    timing.configure_ms.push(milliseconds(began));
    private.push(worker.private_bytes()?);
    let mut hash = Sha256::new();
    let mut normalized_hash = Sha256::new();
    let mut mapping = crate::audio_timing::AudioTiming::new(worker.generation())?;
    let mut source_next = SOURCE_START;
    let mut trimmed = 0usize;
    let mut pass_delay = None;
    let mut pcm_all = if keep_pcm {
        Vec::with_capacity(PACKETS * fixture.samples_per_frame * CHANNELS)
    } else {
        Vec::new()
    };
    for (index, packet) in packets(fixture.bytes)?.into_iter().enumerate() {
        let began = Instant::now();
        let decoded = worker.decode_block(packet);
        timing.decode_ms.push(milliseconds(began));
        let decoded = decoded.map_err(|e| decoder_failure(worker, e))?;
        let block_timing = mapping.map_block(
            decoded.generation,
            SOURCE_START.wrapping_add((index * fixture.samples_per_frame) as u32),
            decoded.pcm.len() / CHANNELS,
            decoded.additional_delay_samples,
        )?;
        pass_delay = Some(decoded.additional_delay_samples);
        trimmed += block_timing.skip_frames;
        if block_timing.output_frames != 0 {
            ensure!(
                block_timing.output_rtp == source_next,
                "normalized source-clock discontinuity"
            );
            source_next = source_next.wrapping_add(block_timing.output_frames as u32);
        }
        let normalized = &decoded.pcm[block_timing.skip_frames * CHANNELS..];
        for sample in normalized {
            normalized_hash.update(sample.to_le_bytes());
        }
        if keep_pcm {
            timing.normalized_pcm.extend_from_slice(normalized);
        }
        if index == 0 && timing.reported_delay_samples.is_none() {
            timing.reported_delay_samples = worker.decoder_delay()?;
        }
        ensure!(
            decoded.pcm.len() == fixture.samples_per_frame * CHANNELS,
            "{} packet{}: expected{} interleaved samples, received{}",
            fixture.name,
            index,
            fixture.samples_per_frame * CHANNELS,
            decoded.pcm.len()
        );
        for sample in &decoded.pcm {
            hash.update(sample.to_le_bytes());
        }
        if keep_pcm {
            pcm_all.extend(decoded.pcm);
        }
        if index % 32 == 31 {
            private.push(worker.private_bytes()?);
        }
    }
    private.push(worker.private_bytes()?);
    ensure!(
        private.iter().all(|value| *value <= WORKER_LIMIT),
        "observed worker private bytes exceed job limit"
    );
    let delay = pass_delay.context("missing additional decoder delay")?;
    ensure!(
        trimmed == delay as usize
            && source_next == SOURCE_START + (PACKETS * fixture.samples_per_frame) as u32 - delay,
        "normalized source interval"
    );
    ensure!(
        timing
            .additional_delay_samples
            .is_none_or(|old| old == delay),
        "reset changed additional decoder delay"
    );
    timing.additional_delay_samples = Some(delay);
    let normalized_hash = format!("{:x}", normalized_hash.finalize());
    ensure!(
        timing
            .normalized_hashes
            .first()
            .is_none_or(|old| old == &normalized_hash),
        "reset changed normalized PCM"
    );
    timing.normalized_hashes.push(normalized_hash);
    timing.pass_generations.push(worker.generation());
    timing.pass_delay_samples.push(delay);
    Ok((format!("{:x}", hash.finalize()), pcm_all))
}
fn new_file(path: &Path, bytes: &[u8]) -> Result<()> {
    let mut file = OpenOptions::new().create_new(true).write(true).open(path)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    Ok(())
}
fn save_pcm(directory: &Path, fixture: &Fixture, pcm: &[i16]) -> Result<()> {
    ensure!(
        pcm.len() == PACKETS * fixture.samples_per_frame * CHANNELS,
        "PCM output count"
    );
    let bytes: Vec<u8> = pcm.iter().flat_map(|sample| sample.to_le_bytes()).collect();
    new_file(&directory.join(format!("{}.pcm", fixture.name)), &bytes)
}
fn control_after_guard(worker: &mut native::Worker, fixture: &Fixture, expected: &str) -> bool {
    let mut timing = CodecReport::default();
    let mut private = Vec::new();
    decode_fixture(worker, fixture, &mut timing, &mut private, false)
        .is_ok_and(|(hash, _)| hash == expected)
}
fn guards(fixture: &Fixture, expected: &str) -> Result<Vec<GuardReport>> {
    let first = packets(fixture.bytes)?[0];
    let mut mutated = first.to_vec();
    for index in [0, first.len() / 3, first.len() / 2, first.len() - 1] {
        mutated[index] ^= 0xff;
    }
    let cases = [
        ("empty", Vec::new(), true),
        ("oversized", vec![0; 8193], true),
        ("truncated_byte", first[..1].to_vec(), false),
        ("truncated_half", first[..first.len() / 2].to_vec(), false),
        ("truncated_tail", first[..first.len() - 1].to_vec(), false),
        ("mutated", mutated, false),
        ("maximum_zeros", vec![0; 8192], false),
    ];
    let mut reports = Vec::new();
    for (name, packet, require_rejection) in cases {
        let mut worker = native::Worker::start()?;
        worker.configure(fixture.codec)?;
        let began = Instant::now();
        let result = worker.decode(&packet);
        let elapsed_ms = milliseconds(began);
        let rejected = result.is_err();
        let output_bounded = match &result {
            Ok(pcm) => !pcm.is_empty() && pcm.len() <= 4096 && pcm.len().is_multiple_of(2),
            Err(_) => true,
        };
        let diagnostic = result
            .as_ref()
            .err()
            .map(brief)
            .unwrap_or_else(|| "bounded output/concealment accepted".into());
        // Invalid worker-contract requests intentionally terminate the child.
        // Other malformed codec records may be rejected or concealed; neither
        // outcome excuses failure to decode a known control after reconfigure.
        let mut recovered = control_after_guard(&mut worker, fixture, expected);
        if !recovered {
            worker = native::Worker::start()?;
            recovered = control_after_guard(&mut worker, fixture, expected);
        }
        reports.push(GuardReport {
            codec: fixture.name,
            name,
            rejected,
            bounded: output_bounded && elapsed_ms < 3000.0 && (!require_rejection || rejected),
            recovered,
            elapsed_ms,
            diagnostic,
        });
    }
    let mut worker = native::Worker::start()?;
    let began = Instant::now();
    let rejected = worker.configure(255).is_err();
    let elapsed_ms = milliseconds(began);
    let mut fresh = native::Worker::start()?;
    reports.push(GuardReport {
        codec: fixture.name,
        name: "unsupported_config",
        rejected,
        bounded: rejected && elapsed_ms < 3000.0,
        recovered: control_after_guard(&mut fresh, fixture, expected),
        elapsed_ms,
        diagnostic: "unsupported configuration must fail closed".into(),
    });
    Ok(reports)
}
fn experiment(directory: &Path, report: &mut Report) -> Result<()> {
    // The same zero-capability process and denial checks as the normal silent
    // probe. Drop this worker before measuring fresh session lifetimes.
    drop(native::checked_worker()?);
    let mut usable = [false; 4];
    for (index, fixture) in FIXTURES.iter().enumerate() {
        let mut worker = native::Worker::start()?;
        let result = decode_fixture(
            &mut worker,
            fixture,
            &mut report.codecs[index],
            &mut Vec::new(),
            true,
        );
        match result {
            Ok((hash, pcm)) => {
                if report.backend.to_ascii_lowercase().contains("fdk")
                    && let Some(expected) = fixture.fdk_hash
                {
                    ensure!(
                        hash == expected,
                        "{} FDK baseline golden changed",
                        fixture.name
                    );
                }
                save_pcm(directory, fixture, &pcm)?;
                let normalized_bytes: Vec<u8> = report.codecs[index]
                    .normalized_pcm
                    .iter()
                    .flat_map(|sample| sample.to_le_bytes())
                    .collect();
                new_file(
                    &directory.join(format!("{}.normalized.pcm", fixture.name)),
                    &normalized_bytes,
                )?;
                report.codecs[index].normalized_pcm.clear();
                report.codecs[index].hash = hash.clone();
                report.codecs[index].hashes.push(hash);
                usable[index] = true;
            }
            Err(error) => report.codecs[index].error = brief(&error),
        }
    }
    for index in 0..report.sessions_requested {
        let began = Instant::now();
        let mut worker = native::Worker::start()?;
        let mut session = SessionReport {
            index,
            wall_ms: 0.0,
            worker_cpu_ms: None,
            private_bytes: vec![worker.private_bytes()?],
            error: String::new(),
        };
        for codec_index in [0, 1, 2, 3, 3, 2, 1, 0] {
            if !usable[codec_index] {
                continue;
            }
            let fixture = &FIXTURES[codec_index];
            let result = decode_fixture(
                &mut worker,
                fixture,
                &mut report.codecs[codec_index],
                &mut session.private_bytes,
                false,
            );
            match result {
                Ok((hash, _)) if hash == report.codecs[codec_index].hash => {
                    report.codecs[codec_index].hashes.push(hash)
                }
                Ok(_) => {
                    session.error = format!("{} reset/session hash changed", fixture.name);
                    break;
                }
                Err(error) => {
                    session.error = brief(&error);
                    break;
                }
            }
        }
        session.worker_cpu_ms = worker.cpu_time_ms().ok();
        session.wall_ms = milliseconds(began);
        report.sessions.push(session);
    }
    for (index, fixture) in FIXTURES[..2].iter().enumerate() {
        if usable[index] {
            report
                .guards
                .extend(guards(fixture, &report.codecs[index].hash)?);
        }
    }
    ensure!(
        usable.iter().all(|value| *value),
        "one or more required AAC formats failed; see report.json"
    );
    ensure!(
        report
            .sessions
            .iter()
            .all(|s| s.error.is_empty() && s.worker_cpu_ms.is_some()),
        "session stability/CPU measurement failed"
    );
    ensure!(
        report.guards.iter().all(|g| g.bounded && g.recovered),
        "malformed-input containment or control recovery failed"
    );
    Ok(())
}
fn quoted(value: &str) -> String {
    let mut out = String::from("\"");
    for character in value.chars() {
        match character {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c.is_control() => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}
fn numbers<T: ToString>(values: &[T]) -> String {
    format!(
        "[{}]",
        values
            .iter()
            .map(ToString::to_string)
            .collect::<Vec<_>>()
            .join(",")
    )
}
fn report_json(report: &Report) -> String {
    let codecs = FIXTURES.iter().zip(&report.codecs).map(|(fixture, result)| {
        let delay = result.additional_delay_samples.unwrap_or(0);
        let normalized = format!("{{\"pcm_file\":{},\"pcm_sha256\":{},\"samples_i16\":{},\"start_sample\":{SOURCE_START},\"end_sample_exclusive\":{},\"trimmed_leading_samples\":{delay},\"additional_delay_samples\":{delay},\"configure_generation\":{},\"pass_hashes\":[{}],\"pass_generations\":{},\"pass_delay_samples\":{}}}",
            quoted(&format!("{}.normalized.pcm", fixture.name)), quoted(result.normalized_hashes.first().map_or("", String::as_str)),
            (PACKETS * fixture.samples_per_frame - delay as usize) * CHANNELS,
            SOURCE_START + (PACKETS * fixture.samples_per_frame) as u32 - delay,
            result.pass_generations.first().unwrap_or(&0),
            result.normalized_hashes.iter().map(|value| quoted(value)).collect::<Vec<_>>().join(","),
            numbers(&result.pass_generations), numbers(&result.pass_delay_samples));
        format!("{{\"codec\":{},\"signal\":{},\"sample_rate\":{RATE},\"channels\":{CHANNELS},\"frames\":{PACKETS},\"samples_per_frame\":{},\"pcm_file\":{},\"pcm_sha256\":{},\"samples_i16\":{},\"pass_hashes\":[{}],\"decode_ms\":{},\"configure_ms\":{},\"reported_delay_samples\":{},\"normalized\":{normalized},\"error\":{}}}",
            quoted(fixture.name), quoted(fixture.signal), fixture.samples_per_frame, quoted(&format!("{}.pcm", fixture.name)), quoted(&result.hash), PACKETS * fixture.samples_per_frame * CHANNELS,
            result.hashes.iter().map(|value| quoted(value)).collect::<Vec<_>>().join(","), numbers(&result.decode_ms), numbers(&result.configure_ms), result.reported_delay_samples.map_or_else(|| "null".into(), |value| value.to_string()), quoted(&result.error))
    }).collect::<Vec<_>>().join(",");
    let sessions = report.sessions.iter().map(|session| format!("{{\"index\":{},\"wall_ms\":{},\"worker_cpu_ms\":{},\"private_bytes\":{},\"error\":{}}}",
        session.index, session.wall_ms, session.worker_cpu_ms.map_or_else(|| "null".into(), |v| v.to_string()), numbers(&session.private_bytes), quoted(&session.error))).collect::<Vec<_>>().join(",");
    let guards = report.guards.iter().map(|guard| format!("{{\"codec\":{},\"name\":{},\"rejected\":{},\"bounded\":{},\"recovered\":{},\"elapsed_ms\":{},\"diagnostic\":{}}}",
        quoted(guard.codec), quoted(guard.name), guard.rejected, guard.bounded, guard.recovered, guard.elapsed_ms, quoted(&guard.diagnostic))).collect::<Vec<_>>().join(",");
    format!(
        "{{\"schema_version\":2,\"fixture_clock\":{{\"sample_rate\":{RATE},\"start_sample\":{SOURCE_START},\"semantics\":\"compressed-packet-source-samples\"}},\"backend\":{},\"sessions_requested\":{},\"audio_samples_submitted\":0,\"synthetic_fixtures_only\":true,\"worker_memory_limit_bytes\":{WORKER_LIMIT},\"success\":{},\"failure\":{},\"codecs\":[{codecs}],\"sessions\":[{sessions}],\"guards\":[{guards}]}}\n",
        quoted(&report.backend),
        report.sessions_requested,
        report.failure.is_empty(),
        quoted(&report.failure)
    )
}
fn output_directory(path: &Path) -> Result<PathBuf> {
    // Require an existing parent beneath ignored local/. This keeps the
    // synthetic dumps out of source, user-media directories and deployed builds.
    let allowed = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("local")
        .canonicalize()
        .context("create the experiment parent below repository local/ first")?;
    ensure!(
        path.is_absolute(),
        "experiment output must be an absolute path"
    );
    let parent = path
        .parent()
        .context("experiment output parent")?
        .canonicalize()?;
    ensure!(
        parent.starts_with(&allowed),
        "experiment output must stay below repository local/"
    );
    let name = path
        .file_name()
        .context("experiment output directory name")?;
    let output = parent.join(name);
    fs::create_dir(&output).context("experiment output directory must be new")?;
    Ok(output)
}
pub fn run(args: &[String]) -> Result<()> {
    let (mut output, mut sessions) = (None, 10usize);
    let mut index = 0;
    while index < args.len() {
        match args[index].as_str() {
            "--experiment-out" => {
                ensure!(output.is_none(), "duplicate experiment output");
                index += 1;
                output = Some(PathBuf::from(
                    args.get(index).context("--experiment-out requires path")?,
                ));
            }
            "--experiment-sessions" => {
                index += 1;
                sessions = args
                    .get(index)
                    .context("--experiment-sessions requires number")?
                    .parse()?;
            }
            _ => anyhow::bail!("unknown decoder experiment option"),
        }
        index += 1;
    }
    ensure!(
        (1..=50).contains(&sessions),
        "experiment sessions must be 1..50"
    );
    let directory = output_directory(&output.context("--experiment-out is required")?)?;
    let mut report = Report {
        backend: native::aac_backend(),
        sessions_requested: sessions,
        codecs: std::array::from_fn(|_| CodecReport::default()),
        sessions: Vec::new(),
        guards: Vec::new(),
        failure: String::new(),
    };
    let result = experiment(&directory, &mut report);
    if let Err(error) = &result {
        report.failure = brief(error);
    }
    let json = report_json(&report);
    ensure!(json.len() <= 2 * 1024 * 1024, "experiment report bound");
    new_file(&directory.join("report.json"), json.as_bytes())?;
    println!(
        "Decoder experiment {}: {}. Synthetic PCM only; no audio endpoint opened.",
        report.backend,
        if result.is_ok() {
            "PASS"
        } else {
            "FAIL (see report.json)"
        }
    );
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn embedded_fixture_framing_is_exact_and_truncations_fail() {
        for fixture in &FIXTURES {
            assert_eq!(packets(fixture.bytes).unwrap().len(), PACKETS);
            assert!(packets(&fixture.bytes[..fixture.bytes.len() - 1]).is_err());
        }
        assert!(packets(&[0xff; 4]).is_err());
        assert!(packets(&[0; 4]).is_err());
    }
    #[test]
    fn report_strings_escape_control_and_path_characters() {
        assert_eq!(quoted("a\"\\\n\0"), "\"a\\\"\\\\\\n\\u0000\"");
    }
}
