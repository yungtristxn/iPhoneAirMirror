"""Deterministic negative controls for the silent waveform/report analyzer."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("decoder_comparison", ROOT / "scripts/Test-DecoderExperiment.py")
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


def noise():
    random = np.random.default_rng(0xA1A1)
    source = random.normal(0, 1000, (57600, 2))
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


if __name__ == "__main__":
    unittest.main()
