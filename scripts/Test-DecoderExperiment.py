"""Silent synthetic AAC backend comparison; never opens or plays an audio device.

Requires the already-installed development NumPy package. No package installation
or application build is performed. Steady tones do not establish sample delay;
only the seeded nonperiodic fixtures qualify temporal alignment.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import time
from datetime import datetime, timezone

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = {"eld": (480, "periodic"), "lc": (1024, "periodic"),
            "eld-experiment": (480, "nonperiodic"), "lc-experiment": (1024, "nonperiodic")}
FDK_HASHES = {
    "eld": "c5679c95ef23189c71327d0e78e26ffd94c596f40902ccdfb7f5feaf256f8f6f",
    "lc": "ec0e0ecf994a43665ab2ef72c245ca49385d5927353a531e97dbe84707b59ae9",
}
RATE, CHANNELS, FRAMES = 44100, 2, 120
FIXTURE_CLOCK = {"sample_rate": RATE, "start_sample": 441000,
                 "semantics": "compressed-packet-source-samples"}
ADDITIONAL_DSP_DELAY = {"fdk": {"eld": 0, "lc": 1685},
                        "ffmpeg": {"eld": 0, "lc": 0}}
MEMORY_LIMIT = 256 * 1024 * 1024
TOLERANCES = {
    "correlation_min": 0.995, "normalized_rmse_max": 0.10,
    "rms_ratio_min": 0.95, "rms_ratio_max": 1.05,
    "peak_error_fraction_max": 0.25, "peak_error_floor_i16": 64,
    "nonperiodic_alignment_max_samples": 44,  # approximately one millisecond
    "nonperiodic_peak_margin_min": 0.05,
    "onset_alignment_disagreement_max_samples": 128,
}
GUARDS = {"empty", "oversized", "truncated_byte", "truncated_half", "truncated_tail",
          "mutated", "maximum_zeros", "unsupported_config"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def finite_values(values, label: str) -> np.ndarray:
    require(isinstance(values, list) and len(values) <= 60000, f"{label}: invalid bounded array")
    array = np.asarray(values, dtype=np.float64)
    require(array.ndim == 1 and np.isfinite(array).all() and (array >= 0).all(),
            f"{label}: invalid numeric measurement")
    return array


def distribution(values) -> dict:
    values = np.asarray(values, dtype=np.float64)
    if not len(values):
        return {"count": 0, "p50": None, "p95": None, "max": None}
    return {"count": len(values), "p50": float(np.percentile(values, 50)),
            "p95": float(np.percentile(values, 95)), "max": float(np.max(values))}


def load_report(path: Path) -> dict:
    with path.open("rb") as source:
        data = source.read(2 * 1024 * 1024 + 1)
    require(len(data) <= 2 * 1024 * 1024, "report exceeds two MiB")
    report = json.loads(data, parse_constant=lambda token: (_ for _ in ()).throw(ValueError(token)))
    require(report.get("schema_version") in (1, 2), "unknown experiment report schema")
    if report["schema_version"] == 2:
        require(report.get("fixture_clock") == FIXTURE_CLOCK,
                "timestamped report must use the fixed fixture source clock")
    require(report.get("audio_samples_submitted") == 0 and report.get("synthetic_fixtures_only") is True,
            "report must identify synthetic-only decoding without endpoint submission")
    require(1 <= report.get("sessions_requested", 0) <= 50, "invalid session count")
    require(isinstance(report.get("codecs"), list) and len(report["codecs"]) == 4,
            "report must cover all four fixed fixtures")
    require({entry.get("codec") for entry in report["codecs"]} == set(FIXTURES),
            "fixture identities do not match")
    require(len(report.get("sessions", [])) <= 50 and len(report.get("guards", [])) <= 32,
            "report resource bound")
    if report.get("success") is True:
        require(len(report["sessions"]) == report["sessions_requested"], "successful report is missing sessions")
        expected_passes = 1 + 2 * report["sessions_requested"]
        for entry in report["codecs"]:
            require(len(entry.get("pass_hashes", [])) == expected_passes,
                    "successful report is missing reset/session hashes")
            require(len(entry.get("decode_ms", [])) == expected_passes * FRAMES,
                    "successful report is missing packet measurements")
    return report


def load_pcm(report_path: Path, entry: dict, baseline: bool = False) -> np.ndarray:
    name = entry["codec"]
    samples_per_frame, signal = FIXTURES[name]
    require(entry.get("signal") == signal, "fixture signal metadata")
    require(entry.get("sample_rate") == RATE and entry.get("channels") == CHANNELS,
            "decoded audio format mismatch")
    expected_samples = FRAMES * samples_per_frame * CHANNELS
    require(entry.get("frames") == FRAMES and entry.get("samples_per_frame") == samples_per_frame
            and entry.get("samples_i16") == expected_samples, "decoded sample count mismatch")
    require(entry.get("pcm_file") == f"{name}.pcm", "unexpected PCM output name")
    path = report_path.parent / entry["pcm_file"]
    require(path.resolve().parent == report_path.parent.resolve(), "PCM file escaped report directory")
    with path.open("rb") as source:
        data = source.read(expected_samples * 2 + 1)
    require(len(data) == expected_samples * 2, "PCM file length mismatch")
    digest = hashlib.sha256(data).hexdigest()
    require(digest == entry.get("pcm_sha256"), "PCM file/hash mismatch")
    if baseline and name in FDK_HASHES:
        require(digest == FDK_HASHES[name], "FDK baseline differs from pinned PCM golden")
    hashes = entry.get("pass_hashes", [])
    require(isinstance(hashes, list) and 1 <= len(hashes) <= 101
            and all(value == digest for value in hashes), "codec reset/session output changed")
    return np.frombuffer(data, dtype="<i2").astype(np.float64).reshape(-1, CHANNELS)


def expected_dsp_delay(backend: str, name: str) -> int:
    require(isinstance(backend, str) and len(backend) <= 256, "invalid backend identity")
    matches = [kind for kind in ADDITIONAL_DSP_DELAY if kind in backend.lower()]
    require(len(matches) == 1, "unknown or ambiguous decoder delay contract")
    return ADDITIONAL_DSP_DELAY[matches[0]][name.split("-", 1)[0]]


def load_normalized_pcm(report_path: Path, report: dict, entry: dict,
                        raw: np.ndarray) -> tuple[np.ndarray, dict]:
    require(report.get("schema_version") == 2 and report.get("fixture_clock") == FIXTURE_CLOCK,
            "normalized qualification requires the fixed source-clock report")
    normalized = entry.get("normalized")
    require(isinstance(normalized, dict), "missing timestamp-normalized output")
    name = entry["codec"]
    delay = expected_dsp_delay(report["backend"], name)
    expected_count = len(raw) - delay
    require(expected_count > 0, "decoder delay exceeds the fixture")
    for field, expected in {
        "additional_delay_samples": delay,
        "trimmed_leading_samples": delay,
        "samples_i16": expected_count * CHANNELS,
        "start_sample": FIXTURE_CLOCK["start_sample"],
        "end_sample_exclusive": FIXTURE_CLOCK["start_sample"] + expected_count,
    }.items():
        require(type(normalized.get(field)) is int and normalized[field] == expected,
                f"normalized {field} violates the declared source-clock/delay contract")
    count = 1 + 2 * report["sessions_requested"]
    generations = normalized.get("pass_generations")
    require(isinstance(generations, list) and len(generations) == count and
            all(type(value) is int and 0 < value < 2 ** 64 for value in generations),
            "missing or invalid per-configure generations")
    require(type(normalized.get("configure_generation")) is int and
            normalized["configure_generation"] == generations[0],
            "normalized generation does not match its configure pass")
    # A worker starts a fresh generation counter. Equality across different
    # sessions is allowed; the runner enforces block ownership within each pass.
    delays = normalized.get("pass_delay_samples")
    require(isinstance(delays, list) and len(delays) == count and
            all(type(value) is int and value == delay for value in delays),
            "stale or changed additional delay across configure/reset passes")
    require(normalized.get("pcm_file") == f"{name}.normalized.pcm", "unexpected normalized PCM output name")
    path = report_path.parent / normalized["pcm_file"]
    require(path.resolve().parent == report_path.parent.resolve(), "normalized PCM escaped report directory")
    with path.open("rb") as source:
        data = source.read(expected_count * CHANNELS * 2 + 1)
    require(len(data) == expected_count * CHANNELS * 2, "normalized PCM length mismatch")
    digest = hashlib.sha256(data).hexdigest()
    require(digest == normalized.get("pcm_sha256"), "normalized PCM hash mismatch")
    hashes = normalized.get("pass_hashes")
    require(isinstance(hashes, list) and len(hashes) == count and all(value == digest for value in hashes),
            "normalized reset/session output changed or passes are missing")
    pcm = np.frombuffer(data, dtype="<i2").astype(np.float64).reshape(-1, CHANNELS)
    # Independently verify the runner used the declared metadata, not a fitted
    # cross-correlation offset, padding or a silently altered waveform.
    require(np.array_equal(pcm, raw[delay:]), "normalized PCM does not equal the declared leading trim of raw output")
    return pcm, normalized


def overlap(reference: np.ndarray, candidate: np.ndarray, lag: int):
    # Positive lag means the candidate output is delayed relative to reference.
    ref_start, candidate_start = max(0, -lag), max(0, lag)
    count = min(len(reference) - ref_start, len(candidate) - candidate_start)
    require(count > 0, "no waveform overlap")
    return reference[ref_start:ref_start + count], candidate[candidate_start:candidate_start + count]


def estimate_lag(reference: np.ndarray, candidate: np.ndarray, max_lag: int) -> dict:
    require(reference.shape[1] == candidate.shape[1] == CHANNELS, "stereo input required")
    require(max_lag < min(len(reference), len(candidate)) // 4, "alignment search exceeds bounded window")
    x, y = reference - reference.mean(axis=0), candidate - candidate.mean(axis=0)
    transform_size = 1 << (len(x) + len(y) - 1).bit_length()
    product = np.fft.rfft(y, n=transform_size, axis=0) * np.conj(np.fft.rfft(x, n=transform_size, axis=0))
    correlation = np.fft.irfft(product, n=transform_size, axis=0).sum(axis=1)
    x_energy = np.concatenate(([0.0], np.cumsum(np.sum(x * x, axis=1))))
    y_energy = np.concatenate(([0.0], np.cumsum(np.sum(y * y, axis=1))))
    lags = np.arange(-max_lag, max_lag + 1, dtype=np.int64)
    x_start, y_start = np.maximum(0, -lags), np.maximum(0, lags)
    count = np.minimum(len(x) - x_start, len(y) - y_start)
    denominator = np.sqrt((x_energy[x_start + count] - x_energy[x_start]) *
                          (y_energy[y_start + count] - y_energy[y_start]))
    scores = np.divide(correlation[lags % transform_size], denominator,
                       out=np.zeros_like(denominator), where=denominator > 0)
    best = float(scores.max())
    ties = np.flatnonzero(scores >= best - 1e-10)
    selected = int(ties[np.argmin(np.abs(lags[ties]))])
    lag = int(lags[selected])
    alternatives = scores[np.abs(lags - lag) > 64]
    margin = best - float(alternatives.max()) if len(alternatives) else 0.0
    return {"lag_samples": lag, "lag_ms": lag * 1000 / RATE,
            "correlation_peak": best, "peak_margin_outside_64_samples": margin}


def onset(samples: np.ndarray) -> int | None:
    energy = np.mean(samples * samples, axis=1)
    prefix = np.concatenate(([0.0], np.cumsum(energy)))
    window = 64
    power = (prefix[window:] - prefix[:-window]) / window
    if not len(power) or power.max() <= 0:
        return None
    active = np.flatnonzero(power >= power.max() * 0.0064)  # eight percent of peak window RMS
    return int(active[0]) if len(active) else None


def peak_frequency(samples: np.ndarray) -> float:
    centered = samples - samples.mean()
    magnitude = np.abs(np.fft.rfft(centered * np.hanning(len(centered))))
    index = int(np.argmax(magnitude[1:])) + 1
    adjustment = 0.0
    if 0 < index < len(magnitude) - 1:
        a, b, c = np.log(np.maximum(magnitude[index - 1:index + 2], 1e-30))
        denominator = a - 2 * b + c
        if abs(denominator) > 1e-12:
            adjustment = float(0.5 * (a - c) / denominator)
    return (index + adjustment) * RATE / len(samples)


def compare_waveforms(reference: np.ndarray, candidate: np.ndarray, name: str,
                      *, align_quality: bool = True) -> dict:
    samples_per_frame, signal = FIXTURES[name]
    alignment = estimate_lag(reference, candidate, 4 * samples_per_frame)
    lag = alignment["lag_samples"]
    x, y = overlap(reference, candidate, lag if align_quality else 0)
    trim = min(6 * samples_per_frame, len(x) // 10)
    x, y = x[trim:-trim], y[trim:-trim]
    channels = []
    for channel in range(CHANNELS):
        ref, cand = x[:, channel], y[:, channel]
        ref_rms, cand_rms = float(np.sqrt(np.mean(ref * ref))), float(np.sqrt(np.mean(cand * cand)))
        require(ref_rms > 0, "reference waveform has no energy")
        error = cand - ref
        ref_centered, cand_centered = ref - ref.mean(), cand - cand.mean()
        denominator = float(np.sqrt(np.mean(ref_centered * ref_centered) * np.mean(cand_centered * cand_centered)))
        correlation = float(np.mean(ref_centered * cand_centered)) / denominator if denominator > 0 else 0.0
        normalized_rmse = float(np.sqrt(np.mean(error * error))) / ref_rms
        ratio = cand_rms / ref_rms
        peak_error, peak_ref = float(np.max(np.abs(error))), float(np.max(np.abs(ref)))
        metric = {"channel": channel, "reference_rms_i16": ref_rms, "candidate_rms_i16": cand_rms,
                  "rms_ratio": ratio, "correlation": correlation, "normalized_rmse": normalized_rmse,
                  "max_abs_error_i16": peak_error, "reference_peak_i16": peak_ref,
                  "dc_difference_i16": float(error.mean())}
        passed = (correlation >= TOLERANCES["correlation_min"] and
                  normalized_rmse <= TOLERANCES["normalized_rmse_max"] and
                  TOLERANCES["rms_ratio_min"] <= ratio <= TOLERANCES["rms_ratio_max"] and
                  peak_error <= max(TOLERANCES["peak_error_floor_i16"],
                                    TOLERANCES["peak_error_fraction_max"] * peak_ref))
        if signal == "periodic":
            expected_frequency = 660.0 if name == "lc" and channel == 1 else 440.0
            ideal_rms = 1000 / math.sqrt(3 if name == "lc" else 2)
            metric["frequency_hz"] = peak_frequency(cand)
            metric["source_control_pass"] = (abs(metric["frequency_hz"] - expected_frequency) <= 2.0
                                               and 0.8 * ideal_rms <= cand_rms <= 1.2 * ideal_rms)
            passed = passed and metric["source_control_pass"]
        metric["pass"] = bool(passed)
        channels.append(metric)
    ref_onset, candidate_onset = onset(reference), onset(candidate)
    alignment["reference_onset_samples"] = ref_onset
    alignment["candidate_onset_samples"] = candidate_onset
    alignment["onset_resolution_samples"] = 64
    onset_delta = None if ref_onset is None or candidate_onset is None else candidate_onset - ref_onset
    alignment["onset_delta_samples"] = onset_delta
    if signal == "nonperiodic":
        alignment["unambiguous"] = alignment["peak_margin_outside_64_samples"] >= TOLERANCES["nonperiodic_peak_margin_min"]
        alignment["pass"] = bool(alignment["unambiguous"] and
                                 abs(lag) <= TOLERANCES["nonperiodic_alignment_max_samples"] and
                                 onset_delta is not None and
                                 abs(onset_delta - lag) <= TOLERANCES["onset_alignment_disagreement_max_samples"])
    else:
        alignment["unambiguous"] = False
        alignment["pass"] = None
        alignment["limitation"] = "Periodic correlation is phase-ambiguous; this fixture does not establish decoder delay."
    return {"signal": signal, "alignment": alignment, "channels": channels,
            "quality_alignment_applied_samples": lag if align_quality else 0,
            "aligned_waveform_pass": all(channel["pass"] for channel in channels),
            "pass": all(channel["pass"] for channel in channels) and alignment["pass"] is not False}


def compare_source_intervals(reference: np.ndarray, reference_metadata: dict,
                             candidate: np.ndarray, candidate_metadata: dict, name: str) -> dict:
    require(reference_metadata["start_sample"] == candidate_metadata["start_sample"] == FIXTURE_CLOCK["start_sample"],
            "normalized outputs do not start at the fixed source clock")
    start = max(reference_metadata["start_sample"], candidate_metadata["start_sample"])
    end = min(reference_metadata["end_sample_exclusive"], candidate_metadata["end_sample_exclusive"])
    count = end - start
    require(count > 0, "normalized source intervals do not overlap")
    require(len(reference) == reference_metadata["end_sample_exclusive"] - reference_metadata["start_sample"] and
            len(candidate) == candidate_metadata["end_sample_exclusive"] - candidate_metadata["start_sample"],
            "normalized interval length does not match PCM")
    ref_start, cand_start = start - reference_metadata["start_sample"], start - candidate_metadata["start_sample"]
    # Crop using declared SOURCE timestamps only. The independent lag estimate
    # below is diagnostic/gating evidence and is never applied to these samples.
    item = compare_waveforms(reference[ref_start:ref_start + count], candidate[cand_start:cand_start + count],
                             name, align_quality=False)
    item["common_source_interval"] = {"start_sample": start, "end_sample_exclusive": end,
                                       "samples_per_channel": count,
                                       "reference_uncompared_tail_samples": len(reference) - ref_start - count,
                                       "candidate_uncompared_tail_samples": len(candidate) - cand_start - count}
    item["source_clock"] = FIXTURE_CLOCK
    item["reference_additional_delay_samples"] = reference_metadata["additional_delay_samples"]
    item["candidate_additional_delay_samples"] = candidate_metadata["additional_delay_samples"]
    item["normalization_scope"] = "Declared additional DSP delay was subtracted from packet source timestamps; samples before the source epoch were discarded. No silence padding or correlation-derived correction is applied. Shared codec priming and physical output latency are not calibrated."
    return item


def resource_summary(report: dict) -> dict:
    sessions = report["sessions"]
    peaks, cpu, wall = [], [], []
    for entry in sessions:
        measurements = finite_values(entry.get("private_bytes", []), "private bytes")
        require(len(measurements) > 0, "missing worker memory samples")
        peaks.append(float(measurements.max()))
        if entry.get("worker_cpu_ms") is not None:
            cpu.extend(finite_values([entry["worker_cpu_ms"]], "worker CPU"))
        wall.extend(finite_values([entry["wall_ms"]], "session wall"))
    growth = None
    slope = None
    if len(peaks) >= 6:
        growth = float(np.median(peaks[-3:]) - np.median(peaks[:3]))
        slope = float(np.polyfit(np.arange(len(peaks)), peaks, 1)[0])
    trend_pass = growth is None or growth <= 8 * 1024 * 1024 or slope <= 512 * 1024
    return {"observed_private_peak_bytes": max(peaks) if peaks else None,
            "session_peak_growth_bytes": growth, "session_peak_slope_bytes": slope,
            "worker_cpu_ms": distribution(cpu), "session_wall_ms": distribution(wall),
            "first_session_worker_cpu_ms": cpu[0] if len(cpu) else None,
            "remaining_sessions_worker_cpu_ms": distribution(cpu[1:]),
            "memory_pass": bool(peaks and max(peaks) <= MEMORY_LIMIT and trend_pass),
            "measurement_limit": "Worker private-byte snapshots, not an unsampled peak or an indefinite leak proof; host process memory is not included.",
            "cpu_measurement_limit": "GetProcessTimes has 100-ns units, but actual accounting granularity can produce tick-quantized or zero CPU for short workers. These are per-session CPU totals, not per-packet CPU durations or zero-cost claims."}


def guards_pass(report: dict) -> bool:
    entries = report.get("guards", [])
    expected = {(codec, name) for codec in ["eld", "lc"] for name in GUARDS}
    if len(entries) != len(expected) or {(entry.get("codec"), entry.get("name")) for entry in entries} != expected:
        return False
    for entry in entries:
        elapsed = entry.get("elapsed_ms")
        if (entry.get("bounded") is not True or entry.get("recovered") is not True or
                not isinstance(elapsed, (int, float)) or not math.isfinite(elapsed) or not 0 <= elapsed < 3000):
            return False
        if entry["name"] in {"empty", "oversized", "unsupported_config"} and entry.get("rejected") is not True:
            return False
    return True


def compare_reports(reference_path: Path, candidate_path: Path) -> dict:
    reference, candidate = load_report(reference_path), load_report(candidate_path)
    require("fdk" in reference["backend"].lower(), "reference backend must be FDK")
    result = {"schema_version": 2, "reference_backend": reference["backend"],
              "candidate_backend": candidate["backend"], "tolerances": TOLERANCES,
              "audio_played": False, "fixtures": {}, "limitations": [
                  "Synthetic decoder/IPC evidence only; not physical audio quality, AirPlay interoperability, A/V sync or glass-to-glass latency.",
                  "Cross-decoder equality is not required; within-backend reset/session determinism is required.",
                  "Timing alignment uses seeded nonperiodic fixtures; periodic phase correlation alone is ambiguous.",
                  "Raw timing/adoption results remain separate; normalization qualifies a defined source-clock mapping, not drop-in raw sample equivalence or physical A/V sync."]}
    by_ref = {entry["codec"]: entry for entry in reference["codecs"]}
    by_cand = {entry["codec"]: entry for entry in candidate["codecs"]}
    for name in FIXTURES:
        try:
            ref_entry, cand_entry = by_ref[name], by_cand[name]
            require(not ref_entry["error"] and not cand_entry["error"],
                    f"decoder error: reference={ref_entry['error']} candidate={cand_entry['error']}")
            ref = load_pcm(reference_path, ref_entry, baseline=True)
            cand = load_pcm(candidate_path, cand_entry)
            item = compare_waveforms(ref, cand, name)
            item["reference_reported_delay_samples"] = ref_entry.get("reported_delay_samples")
            item["candidate_reported_delay_samples"] = cand_entry.get("reported_delay_samples")
            if isinstance(ref_entry.get("reported_delay_samples"), int) and isinstance(cand_entry.get("reported_delay_samples"), int):
                item["reported_delay_difference_samples"] = cand_entry["reported_delay_samples"] - ref_entry["reported_delay_samples"]
            item["delay_metadata_scope"] = "Raw output and its gate are unchanged. The separate normalized gate validates the runner's atomic per-configure delay mapping against a fixed source clock and these raw samples."
            item["bit_exact"] = ref_entry["pcm_sha256"] == cand_entry["pcm_sha256"]
            item["reference_decode_ms"] = distribution(finite_values(ref_entry["decode_ms"], "FDK decode timing"))
            ref_timing = finite_values(ref_entry["decode_ms"], "FDK decode timing")
            timing = finite_values(cand_entry["decode_ms"], "candidate decode timing")
            item["candidate_decode_ms"] = distribution(timing)
            item["reference_first_packet_wall_ms"] = float(ref_timing[0]) if len(ref_timing) else None
            item["candidate_first_packet_wall_ms"] = float(timing[0]) if len(timing) else None
            item["reference_first_packet_per_pass_ms"] = distribution(ref_timing[::FRAMES])
            item["candidate_first_packet_per_pass_ms"] = distribution(timing[::FRAMES])
            item["reference_warm_packet_wall_ms"] = distribution(ref_timing[np.arange(len(ref_timing)) % FRAMES != 0])
            item["candidate_warm_packet_wall_ms"] = distribution(timing[np.arange(len(timing)) % FRAMES != 0])
            item["candidate_configure_ms"] = distribution(finite_values(cand_entry["configure_ms"], "configure timing"))
            item["reference_configure_ms"] = distribution(finite_values(ref_entry["configure_ms"], "FDK configure timing"))
            item["timing_scope"] = "Wall time includes IPC/decode; warm packet statistics exclude packet zero of every freshly configured pass. Process startup is represented by session wall/CPU totals."
            item["frame_budget_ms"] = FIXTURES[name][0] * 1000 / RATE
            item["throughput_pass"] = bool(len(timing) and np.percentile(timing, 95) < item["frame_budget_ms"])
            item["pass"] = item["pass"] and item["throughput_pass"]
            item["raw_pass"] = item["pass"]
            try:
                normalized_ref, reference_metadata = load_normalized_pcm(reference_path, reference, ref_entry, ref)
                normalized_cand, candidate_metadata = load_normalized_pcm(candidate_path, candidate, cand_entry, cand)
                item["normalized"] = compare_source_intervals(normalized_ref, reference_metadata,
                                                               normalized_cand, candidate_metadata, name)
                item["normalized_pass"] = item["normalized"]["pass"] and item["throughput_pass"]
            except (ValueError, KeyError, OSError) as error:
                item["normalized"] = {"pass": False, "error": str(error)}
                item["normalized_pass"] = False
            result["fixtures"][name] = item
        except (ValueError, KeyError, OSError) as error:
            result["fixtures"][name] = {"pass": False, "raw_pass": False,
                                         "normalized_pass": False, "error": str(error)}
    result["reference_resources"] = resource_summary(reference)
    result["candidate_resources"] = resource_summary(candidate)
    result["candidate_guard_pass"] = guards_pass(candidate)
    result["reference_guard_pass"] = guards_pass(reference)
    result["reference_run_pass"] = reference.get("success") is True
    result["candidate_run_pass"] = candidate.get("success") is True
    common_gates = bool(result["reference_run_pass"] and result["candidate_run_pass"] and
        result["candidate_guard_pass"] and result["reference_guard_pass"] and
        result["candidate_resources"]["memory_pass"] and result["reference_resources"]["memory_pass"])
    result["raw_adoption_gate"] = common_gates and all(item["raw_pass"] for item in result["fixtures"].values())
    result["normalized_adoption_gate"] = common_gates and all(item["normalized_pass"] for item in result["fixtures"].values())
    result["qualification_basis"] = "timestamp_normalized_source_interval"
    result["pass"] = result["normalized_adoption_gate"]
    return result


def run_backend(executable: Path, output: Path, sessions: int, timeout: int) -> tuple[Path, dict]:
    executable = executable.resolve(strict=True)
    require(not executable.is_relative_to((ROOT / "dist").resolve()), "experiment must not modify or run the preserved dist build")
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    with output.with_suffix(".stdout.txt").open("xb") as stdout, output.with_suffix(".stderr.txt").open("xb") as stderr:
        provision = subprocess.run([str(executable), "--provision-worker"], stdin=subprocess.DEVNULL,
                                   stdout=stdout, stderr=stderr, timeout=15, creationflags=flags)
        require(provision.returncode == 0, "experimental executable worker provisioning failed")
        began = time.perf_counter()
        process = subprocess.Popen([str(executable), "--decoder-experiment", "--experiment-out", str(output),
                                    "--experiment-sessions", str(sessions)], stdin=subprocess.DEVNULL,
                                   stdout=stdout, stderr=stderr, creationflags=flags)
        try:
            returncode = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            raise RuntimeError("decoder experiment exceeded its process deadline")
        finally:
            if process.poll() is None:
                process.kill()  # Only this test-created process; its worker job closes with it.
                process.wait(timeout=10)
    execution = {"executable": str(executable), "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                 "exit_code": returncode, "wall_ms": (time.perf_counter() - began) * 1000}
    require(returncode == 0, f"decoder process exited with {returncode}; inspect its saved output")
    require((output / "report.json").is_file(), "decoder process produced no report")
    return output / "report.json", execution


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--reference-report", type=Path)
    parser.add_argument("--candidate-report", type=Path)
    parser.add_argument("--sessions", type=int, default=10)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    executables = (args.reference is not None, args.candidate is not None)
    reports = (args.reference_report is not None, args.candidate_report is not None)
    run = all(executables)
    analyze = all(reports)
    require((run and not any(reports)) or (analyze and not any(executables)),
            "supply exactly one complete executable pair or report pair")
    require(1 <= args.sessions <= 50 and 10 <= args.timeout <= 1800, "sessions/timeout bounds")
    allowed = (ROOT / "local").resolve()
    output = (args.output or allowed / "decoder-experiment" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")).resolve()
    require(output.is_relative_to(allowed) and output != allowed, "comparison outputs must stay under repository local/")
    output.mkdir(parents=True, exist_ok=False)
    execution = {}
    if run:
        reference, execution["reference"] = run_backend(args.reference, output / "fdk", args.sessions, args.timeout)
        candidate, execution["candidate"] = run_backend(args.candidate, output / "candidate", args.sessions, args.timeout)
    else:
        reference, candidate = args.reference_report.resolve(strict=True), args.candidate_report.resolve(strict=True)
    result = compare_reports(reference, candidate)
    result["execution"] = execution
    result["reference_report"] = str(reference)
    result["candidate_report"] = str(candidate)
    destination = output / "comparison.json"
    destination.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(f"{'PASS' if result['pass'] else 'FAIL'} normalized source-clock qualification; "
          f"raw adoption gate={'PASS' if result['raw_adoption_gate'] else 'FAIL'}: {destination}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, RuntimeError, OSError, subprocess.SubprocessError) as error:
        print(f"Decoder experiment validation failed: {error}", file=sys.stderr)
        raise SystemExit(1)
