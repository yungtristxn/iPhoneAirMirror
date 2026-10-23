"""Deterministic negative controls for the silent waveform/report analyzer."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("decoder_comparison", ROOT / "scripts/Test-DecoderExperiment.py")
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


def noise(count=57600):
    random = np.random.default_rng(0xA1A1)
    source = random.normal(0, 1000, (count, 2))
    for channel in range(2):
        source[:, channel] = np.convolve(source[:, channel], np.ones(9) / 9, mode="same")
    source[:2048] = 0
    source[-4096:] = 0
    source[(np.arange(len(source)) // 997) % 7 == 0] = 0
    return np.rint(source)


class WaveformControls(unittest.TestCase):
    def test_identical_nonperiodic_data_has_unambiguous_zero_lag(self):
        source = noise()
        result = comparison.compare_waveforms(source, source.copy(), "eld-experiment")
        self.assertTrue(result["pass"])
        self.assertEqual(result["alignment"]["lag_samples"], 0)
        self.assertTrue(result["alignment"]["unambiguous"])

    def test_extra_decoder_delay_is_reported_and_not_hidden_by_alignment(self):
        source = noise()
        delayed = np.zeros_like(source)
        delayed[137:] = source[:-137]
        result = comparison.compare_waveforms(source, delayed, "eld-experiment")
        self.assertEqual(result["alignment"]["lag_samples"], 137)
        self.assertEqual(result["alignment"]["onset_delta_samples"], 137)
        self.assertTrue(result["aligned_waveform_pass"])
        self.assertFalse(result["alignment"]["pass"])
        self.assertFalse(result["pass"])

    def test_small_quantization_variation_does_not_require_bit_exactness(self):
        source = noise()
        candidate = source.copy()
        candidate[2048:-4096:7, 0] += 1
        self.assertTrue(comparison.compare_waveforms(source, candidate, "eld-experiment")["pass"])

    def test_gain_swap_silence_and_spike_controls_fail(self):
        source = noise()
        spike = source.copy()
        spike[20000, 0] = 32767
        for candidate in [source * 1.2, source[:, ::-1], np.zeros_like(source), np.ones_like(source) * 1000, spike]:
            self.assertFalse(comparison.compare_waveforms(source, candidate, "eld-experiment")["pass"])

    def test_periodic_tone_cannot_certify_temporal_alignment(self):
        time = np.arange(57600) / comparison.RATE
        source = np.column_stack([1000 * np.sin(2 * np.pi * 440 * time)] * 2)
        result = comparison.compare_waveforms(source, source, "eld")
        self.assertTrue(result["pass"])
        self.assertFalse(result["alignment"]["unambiguous"])
        self.assertIsNone(result["alignment"]["pass"])
        self.assertIn("ambiguous", result["alignment"]["limitation"])

    def test_raw_lc_failure_survives_correct_source_clock_normalization(self):
        source = noise(120 * 1024)
        delayed = np.zeros_like(source)
        delayed[1685:] = source[:-1685]
        raw = comparison.compare_waveforms(delayed, source, "lc-experiment")
        self.assertFalse(raw["pass"])
        self.assertEqual(raw["alignment"]["lag_samples"], -1685)
        epoch = comparison.FIXTURE_CLOCK["start_sample"]
        reference_metadata = {"start_sample": epoch, "end_sample_exclusive": epoch + len(source) - 1685,
                              "additional_delay_samples": 1685}
        candidate_metadata = {"start_sample": epoch, "end_sample_exclusive": epoch + len(source),
                              "additional_delay_samples": 0}
        result = comparison.compare_source_intervals(delayed[1685:], reference_metadata,
                                                     source, candidate_metadata, "lc-experiment")
        self.assertTrue(result["pass"])
        self.assertEqual(result["alignment"]["lag_samples"], 0)
        self.assertEqual(result["quality_alignment_applied_samples"], 0)
        self.assertEqual(result["common_source_interval"]["candidate_uncompared_tail_samples"], 1685)

    def test_normalized_correlation_independently_rejects_delay_errors(self):
        source = noise(120 * 1024)
        epoch = comparison.FIXTURE_CLOCK["start_sample"]
        metadata = {"start_sample": epoch, "end_sample_exclusive": epoch + len(source),
                    "additional_delay_samples": 0}
        # Forgotten trim, wrong-sign compensation (twice the delay), and a
        # stale LC delay after switching to ELD must not be fitted away.
        for name, offset in [("lc-experiment", 1685), ("lc-experiment", 3370),
                             ("eld-experiment", -1685), ("lc-experiment", 45)]:
            shifted = np.zeros_like(source)
            if offset > 0:
                shifted[offset:] = source[:-offset]
            else:
                shifted[:offset] = source[-offset:]
            result = comparison.compare_source_intervals(source, metadata, shifted, metadata, name)
            self.assertEqual(result["alignment"]["lag_samples"], offset)
            self.assertFalse(result["alignment"]["pass"])
            self.assertFalse(result["pass"])
            self.assertEqual(result["quality_alignment_applied_samples"], 0)

    def test_normalized_source_epoch_cannot_be_shifted_to_hide_error(self):
        source = noise()
        epoch = comparison.FIXTURE_CLOCK["start_sample"]
        metadata = {"start_sample": epoch, "end_sample_exclusive": epoch + len(source),
                    "additional_delay_samples": 0}
        shifted = dict(metadata, start_sample=epoch + 1685, end_sample_exclusive=epoch + len(source) + 1685)
        with self.assertRaisesRegex(ValueError, "fixed source clock"):
            comparison.compare_source_intervals(source, metadata, source, shifted, "eld-experiment")

    def test_cpu_distributions_and_observed_memory_growth(self):
        self.assertEqual(comparison.distribution([1, 2, 3, 4])["p50"], 2.5)
        reports = [{"private_bytes": [10_000_000 + index * 4_000_000],
                    "worker_cpu_ms": 20.0, "wall_ms": 100.0} for index in range(10)]
        summary = comparison.resource_summary({"sessions": reports})
        self.assertFalse(summary["memory_pass"])
        self.assertGreater(summary["session_peak_growth_bytes"], 8 * 1024 * 1024)

    def test_all_guard_identities_and_required_rejections_are_checked(self):
        entries = [{"codec": codec, "name": name, "bounded": True, "recovered": True,
                    "rejected": True, "elapsed_ms": 1.0}
                   for codec in ["eld", "lc"] for name in comparison.GUARDS]
        self.assertTrue(comparison.guards_pass({"guards": entries}))
        entries[0]["recovered"] = False
        self.assertFalse(comparison.guards_pass({"guards": entries}))
        entries[0]["recovered"] = True
        entries[0] = entries[1].copy()
        self.assertFalse(comparison.guards_pass({"guards": entries}))


class FileControls(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "local/decoder-experiment-tests"
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="controls-", dir=parent)
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def normalized_fixture(self, name="lc-experiment", backend="FDK-AAC"):
        raw = noise(comparison.FRAMES * comparison.FIXTURES[name][0])
        delay = comparison.expected_dsp_delay(backend, name)
        data = raw[delay:].astype("<i2").tobytes()
        digest = hashlib.sha256(data).hexdigest()
        epoch = comparison.FIXTURE_CLOCK["start_sample"]
        normalized = {"pcm_file": f"{name}.normalized.pcm", "pcm_sha256": digest,
                      "samples_i16": (len(raw) - delay) * 2, "start_sample": epoch,
                      "end_sample_exclusive": epoch + len(raw) - delay,
                      "additional_delay_samples": delay, "trimmed_leading_samples": delay,
                      "configure_generation": 1, "pass_generations": [1, 1, 5],
                      "pass_delay_samples": [delay] * 3, "pass_hashes": [digest] * 3}
        (self.directory / normalized["pcm_file"]).write_bytes(data)
        report = {"schema_version": 2, "backend": backend, "sessions_requested": 1,
                  "fixture_clock": comparison.FIXTURE_CLOCK.copy()}
        return report, {"codec": name, "normalized": normalized}, raw

    def test_normalized_files_require_exact_trim_clock_delay_and_generation_metadata(self):
        report, entry, raw = self.normalized_fixture()
        path = self.directory / "report.json"
        pcm, metadata = comparison.load_normalized_pcm(path, report, entry, raw)
        self.assertTrue(np.array_equal(pcm, raw[1685:]))
        # Generations may restart across workers; [1, 1, 5] is valid here.
        self.assertEqual(metadata["pass_generations"], [1, 1, 5])
        for field, value in [("additional_delay_samples", -1685), ("trimmed_leading_samples", 0),
                             ("start_sample", 0), ("end_sample_exclusive", metadata["end_sample_exclusive"] + 1),
                             ("configure_generation", 2), ("pass_delay_samples", [1685, 0, 1685]),
                             ("pass_generations", [1, 0, 5]), ("pass_generations", [True, 1, 5]),
                             ("pass_hashes", metadata["pass_hashes"][:-1])]:
            previous = entry["normalized"][field]
            entry["normalized"][field] = value
            with self.assertRaises(ValueError, msg=field):
                comparison.load_normalized_pcm(path, report, entry, raw)
            entry["normalized"][field] = previous

    def test_wrong_trim_cannot_be_authorized_by_rehashing_normalized_pcm(self):
        report, entry, raw = self.normalized_fixture()
        normalized = entry["normalized"]
        # Same advertised size, but discarding the tail instead of priming.
        wrong = raw[:-1685].astype("<i2").tobytes()
        (self.directory / normalized["pcm_file"]).write_bytes(wrong)
        normalized["pcm_sha256"] = hashlib.sha256(wrong).hexdigest()
        normalized["pass_hashes"] = [normalized["pcm_sha256"]] * 3
        with self.assertRaisesRegex(ValueError, "declared leading trim"):
            comparison.load_normalized_pcm(self.directory / "report.json", report, entry, raw)

    def test_stale_lc_delay_is_rejected_for_eld_and_legacy_reports_cannot_qualify(self):
        report, entry, raw = self.normalized_fixture("eld-experiment")
        entry["normalized"]["additional_delay_samples"] = 1685
        with self.assertRaisesRegex(ValueError, "additional_delay_samples"):
            comparison.load_normalized_pcm(self.directory / "report.json", report, entry, raw)
        report["schema_version"] = 1
        with self.assertRaisesRegex(ValueError, "fixed source-clock"):
            comparison.load_normalized_pcm(self.directory / "report.json", report, entry, raw)

    def test_raw_failed_gate_remains_visible_when_normalized_gate_passes(self):
        directories = {backend: self.directory / backend for backend in ["fdk", "ffmpeg"]}
        golden = {}
        for backend, directory in directories.items():
            directory.mkdir()
            entries = []
            for name, (samples_per_frame, signal) in comparison.FIXTURES.items():
                count = comparison.FRAMES * samples_per_frame
                if signal == "nonperiodic":
                    source = noise(count)
                elif name == "eld":
                    time = np.arange(count) / comparison.RATE
                    source = np.column_stack([1000 * np.sin(2 * np.pi * 440 * time)] * 2)
                else:
                    phases = (np.arange(count)[:, None] * np.array([440, 660])) % comparison.RATE
                    source = np.minimum(phases, comparison.RATE - phases) * 2000 / (comparison.RATE / 2) - 1000
                source = np.rint(source)
                delay = comparison.expected_dsp_delay(backend, name)
                raw = source.copy()
                if delay:
                    raw[:delay] = 0
                    raw[delay:] = source[:-delay]
                raw_bytes = raw.astype("<i2").tobytes()
                raw_hash = hashlib.sha256(raw_bytes).hexdigest()
                normalized_bytes = raw[delay:].astype("<i2").tobytes()
                normalized_hash = hashlib.sha256(normalized_bytes).hexdigest()
                (directory / f"{name}.pcm").write_bytes(raw_bytes)
                (directory / f"{name}.normalized.pcm").write_bytes(normalized_bytes)
                if backend == "fdk" and signal == "periodic":
                    golden[name] = raw_hash
                epoch = comparison.FIXTURE_CLOCK["start_sample"]
                entries.append({"codec": name, "signal": signal, "sample_rate": comparison.RATE,
                    "channels": 2, "frames": 120, "samples_per_frame": samples_per_frame,
                    "samples_i16": count * 2, "pcm_file": f"{name}.pcm", "pcm_sha256": raw_hash,
                    "pass_hashes": [raw_hash] * 3, "decode_ms": [0.01] * 360, "configure_ms": [0.1] * 3,
                    "reported_delay_samples": delay, "error": "",
                    "normalized": {"pcm_file": f"{name}.normalized.pcm", "pcm_sha256": normalized_hash,
                        "samples_i16": (count - delay) * 2, "start_sample": epoch,
                        "end_sample_exclusive": epoch + count - delay,
                        "additional_delay_samples": delay, "trimmed_leading_samples": delay,
                        "configure_generation": 1, "pass_generations": [1, 1, 5],
                        "pass_delay_samples": [delay] * 3, "pass_hashes": [normalized_hash] * 3}})
            report = {"schema_version": 2, "fixture_clock": comparison.FIXTURE_CLOCK.copy(),
                "backend": backend, "audio_samples_submitted": 0, "synthetic_fixtures_only": True,
                "sessions_requested": 1, "success": True, "codecs": entries,
                "sessions": [{"private_bytes": [2_000_000], "worker_cpu_ms": 10.0, "wall_ms": 100.0}],
                "guards": [{"codec": codec, "name": name, "bounded": True, "recovered": True,
                    "rejected": True, "elapsed_ms": 1.0} for codec in ["eld", "lc"] for name in comparison.GUARDS]}
            (directory / "report.json").write_text(json.dumps(report), encoding="utf-8")
        # The synthetic analyzer contract fixture supplies its own exact raw
        # golden hashes; this unit test makes no decoder/real-fixture claim.
        with patch.object(comparison, "FDK_HASHES", golden):
            result = comparison.compare_reports(directories["fdk"] / "report.json", directories["ffmpeg"] / "report.json")
        self.assertFalse(result["raw_adoption_gate"])
        self.assertFalse(result["fixtures"]["lc-experiment"]["pass"])
        self.assertEqual(result["fixtures"]["lc-experiment"]["alignment"]["lag_samples"], -1685)
        self.assertTrue(result["normalized_adoption_gate"])
        self.assertTrue(result["pass"])
        self.assertEqual(result["qualification_basis"], "timestamp_normalized_source_interval")
        self.assertEqual(result["fixtures"]["lc-experiment"]["normalized"]["alignment"]["lag_samples"], 0)

    def test_pcm_length_hash_and_path_are_checked(self):
        entry = {"codec": "eld", "signal": "periodic", "sample_rate": 44100,
                 "channels": 2, "frames": 120, "samples_per_frame": 480,
                 "samples_i16": 115200, "pcm_file": "eld.pcm"}
        data = bytes(230400)
        digest = hashlib.sha256(data).hexdigest()
        entry.update(pcm_sha256=digest, pass_hashes=[digest])
        path = self.directory / "eld.pcm"
        path.write_bytes(data)
        self.assertEqual(comparison.load_pcm(self.directory / "report.json", entry).shape, (57600, 2))
        path.write_bytes(data[:-2])
        with self.assertRaisesRegex(ValueError, "length"):
            comparison.load_pcm(self.directory / "report.json", entry)
        path.write_bytes(data)
        entry["pcm_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "hash"):
            comparison.load_pcm(self.directory / "report.json", entry)
        entry["pcm_file"] = "../private.pcm"
        with self.assertRaisesRegex(ValueError, "name"):
            comparison.load_pcm(self.directory / "report.json", entry)

    def test_oversized_json_and_nonfinite_measurements_are_rejected(self):
        path = self.directory / "report.json"
        path.write_bytes(b"0" * (2 * 1024 * 1024 + 1))
        with self.assertRaisesRegex(ValueError, "two MiB"):
            comparison.load_report(path)
        with self.assertRaises(ValueError):
            comparison.finite_values([float("nan")], "decode")
        with self.assertRaises(ValueError):
            comparison.finite_values([-1], "decode")

    def test_success_cannot_omit_repeated_session_hashes(self):
        report = {"schema_version": 1, "audio_samples_submitted": 0,
                  "synthetic_fixtures_only": True, "sessions_requested": 1, "success": True,
                  "codecs": [{"codec": name, "pass_hashes": ["hash"], "decode_ms": []}
                             for name in comparison.FIXTURES], "sessions": [{}], "guards": []}
        path = self.directory / "report.json"
        path.write_text(json.dumps(report), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "hashes"):
            comparison.load_report(path)

    def test_crashed_decoder_cannot_pass_even_when_it_writes_a_report(self):
        executable = self.directory / "fake-lab.exe"
        executable.write_bytes(b"test-only")
        output = self.directory / "crashed"
        output.mkdir()
        (output / "report.json").write_text('{"success": true}', encoding="utf-8")
        process = Mock()
        process.wait.return_value = 1
        process.poll.return_value = 1
        with patch.object(comparison.subprocess, "run", return_value=Mock(returncode=0)), \
                patch.object(comparison.subprocess, "Popen", return_value=process):
            with self.assertRaisesRegex(ValueError, "exited with 1"):
                comparison.run_backend(executable, output, 1, 10)

    def test_interrupted_decoder_is_stopped_before_leaving_the_test(self):
        executable = self.directory / "fake-lab.exe"
        executable.write_bytes(b"test-only")
        process = Mock()
        process.wait.side_effect = [KeyboardInterrupt(), -1]
        process.poll.return_value = None
        with patch.object(comparison.subprocess, "run", return_value=Mock(returncode=0)), \
                patch.object(comparison.subprocess, "Popen", return_value=process):
            with self.assertRaises(KeyboardInterrupt):
                comparison.run_backend(executable, self.directory / "interrupted", 1, 10)
        process.kill.assert_called_once()

    def test_cli_rejects_incomplete_and_mixed_input_pairs_before_launch(self):
        for arguments in [["--reference", "x"], ["--reference-report", "x"],
                          ["--reference", "x", "--candidate", "y", "--reference-report", "z"],
                          ["--reference-report", "x", "--candidate-report", "y", "--candidate", "z"]]:
            with patch.object(comparison.sys, "argv", ["comparison", *arguments]):
                with self.assertRaisesRegex(ValueError, "exactly one complete"):
                    comparison.main()


if __name__ == "__main__":
    unittest.main()
