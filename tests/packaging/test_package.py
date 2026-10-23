"""Silent package-policy checks; no application launch, network or sound."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("release_package", ROOT / "scripts/Package-Share.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackagePolicy(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "local/package-tests"
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=parent)
        self.root = Path(self.temporary.name)
        self.root_patch = patch.object(package, "ROOT", self.root)
        self.root_patch.start()

    def tearDown(self):
        self.root_patch.stop()
        self.temporary.cleanup()

    def test_profiles_exclude_fdk_lab_and_accidental_hap(self):
        for profile, settings in package.PROFILES.items():
            package.validate_features(profile, settings["resolved"])
            for features in [set(), {"ffmpeg-aac"}, settings["resolved"] | {"decoder-lab"}]:
                with self.assertRaisesRegex(RuntimeError, "features"):
                    package.validate_features(profile, features)
        with self.assertRaisesRegex(RuntimeError, "features"):
            package.validate_features("legacy", package.PROFILES["hap"]["resolved"])

    def test_fresh_outputs_cannot_overwrite_or_nest_inside_fallbacks(self):
        candidate = self.root / "dist/candidate"
        self.assertEqual(package.output_path(candidate, directory=True), candidate)
        candidate.mkdir(parents=True)
        marker = candidate / "AirMirrorLite.exe"
        marker.write_bytes(b"preserved")
        for target in [candidate, candidate / "new-copy", self.root / "new-copy"]:
            with self.assertRaises(RuntimeError):
                package.output_path(target, directory=True)
        with self.assertRaisesRegex(RuntimeError, "zip"):
            package.output_path(self.root / "dist/release.exe")
        self.assertEqual(marker.read_bytes(), b"preserved")

    def test_source_selection_excludes_state_fdk_and_traversal(self):
        for name in ["src/main.rs", "docs/STATE.md", "vendor/playfair/upstream-reference/fairplay_playfair.c",
                     "scripts/Package-Share.py", "Cargo.lock"]:
            self.assertTrue(package.included_source(name), name)
        for name in ["local/identity.dpapi", "work/cache.txt", "dist/receiver.log", "vendor/fdk-aac/NOTICE",
                     "/src/file.rs", "src/../../private.txt", "src\\main.rs"]:
            self.assertFalse(package.included_source(name), name)

    def test_snapshot_retains_dirty_edits_omits_deleted_files_and_detects_change(self):
        names = ["Cargo.toml", "Cargo.lock", "build.rs", "LICENSE.md", "NOTICE.md", "src/main.rs", "docs/deleted.md"]
        for name in names[:-1]:
            package.write(self.root / name, name)
        with patch.object(package, "run", return_value="\0".join(names)):
            before = package.source_snapshot()
            self.assertNotIn("docs/deleted.md", before)
            package.write(self.root / "src/main.rs", "modified")
            after = package.source_snapshot()
        self.assertNotEqual(package.snapshot_hash(before), package.snapshot_hash(after))
        self.assertEqual(after["src/main.rs"], hashlib.sha256(b"modified").hexdigest())

    def test_pe_validation_rejects_truncation_machine_and_invalid_offsets(self):
        path = self.root / "probe.exe"
        header = bytearray(70)
        header[:2] = b"MZ"
        struct.pack_into("<I", header, 0x3c, 64)
        header[64:] = b"PE\0\0\x64\x86"
        path.write_bytes(header)
        package.verify_pe(path)
        for bad in [header[:10], header[:64], header[:68] + b"\x4c\x01"]:
            path.write_bytes(bad)
            with self.assertRaises(RuntimeError):
                package.verify_pe(path)
        struct.pack_into("<I", header, 0x3c, 0xffffffff)
        path.write_bytes(header)
        with self.assertRaisesRegex(RuntimeError, "offset"):
            package.verify_pe(path)

    def test_private_names_and_used_local_directories_are_rejected(self):
        stage = self.root / "stage"
        package.write(stage / "local/display-aspect.txt", "994 2160\n")
        self.assertEqual(len(package.package_files(stage)), 1)
        for relative in ["IDENTITY.DPAPI", "nested/receiver.log", "local/unknown.txt", ".git/config"]:
            path = stage / relative
            package.write(path, "private")
            with self.assertRaises(RuntimeError, msg=relative):
                package.package_files(stage)
            path.unlink()

    def test_zip_entries_match_staged_bytes_and_existing_archive_is_preserved(self):
        stage = self.root / "stage"
        package.write(stage / "LICENSE.md", "license\n")
        package.write(stage / "source/src/main.rs", "source\n")
        archive = self.root / "dist/release.zip"
        self.assertEqual(package.archive_package(stage, archive), 2)
        before = archive.read_bytes()
        with zipfile.ZipFile(archive) as result:
            self.assertEqual(result.read("iPhoneAirMirror/source/src/main.rs"), b"source\n")
            self.assertIsNone(result.testzip())
        with self.assertRaisesRegex(RuntimeError, "preserved"):
            package.archive_package(stage, archive)
        self.assertEqual(archive.read_bytes(), before)
        self.assertEqual(list(archive.parent.glob("*.partial-*")), [])


if __name__ == "__main__":
    unittest.main()
