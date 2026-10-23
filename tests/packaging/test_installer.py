"""Silent installer-policy fixtures; these tests never execute an installer."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("installer_package", ROOT / "scripts/Package-Installer.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class InstallerPolicy(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "local/installer-policy-tests"
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=parent)
        self.root = Path(self.temp.name)
        self.scope = patch.object(installer, "ROOT", self.root)
        self.scope.start()
        self.input = self.root / "dist/portable"
        self.record = self.fixture()

    def tearDown(self):
        self.scope.stop()
        self.temp.cleanup()

    def fixture(self):
        files = {"LICENSE.md": "GPL fixture", "NOTICE.md": "notices", "README.txt": "portable",
                 "Start-iPhoneAirMirror.cmd": "launcher", "local/display-aspect.txt": "994 2160\n",
                 "source/Cargo.toml": '[package]\nname="test-only"\nversion="0.1.0-beta.1"\n',
                 "source/Cargo.lock": "lock", "source/build.rs": "build", "source/src/main.rs": "main",
                 "source/native/windows.cpp": "native", "source/native/codec_ffmpeg.cpp": "codec",
                 "source/vendor/playfair/LICENSE.md": "GPL fixture", "source/assets/app-icon.ico": "icon fixture",
                 "source/.cargo/config.toml": "vendored", "source/REBUILD.txt": "build recipe",
                 "source/vendor/rust/crate/src/lib.rs": "crate fixture",
                 "source/vendor/ffmpeg/source.tar.xz": "source archive fixture",
                 "licenses/RUST-DEPENDENCIES.json": "{}",
                 "source/build-info/native-build.json": json.dumps({"target": "x86_64-pc-windows-msvc", "aac": {"backend": "ffmpeg-native-aac"}})}
        for name, value in files.items():
            installer.write(self.input / name, value)
        header = bytearray(70)
        header[:2] = b"MZ"
        struct.pack_into("<I", header, 0x3c, 64)
        header[64:] = b"PE\0\0\x64\x86"
        (self.input / installer.EXECUTABLE).write_bytes(header)
        sources = {name.removeprefix("source/"): installer.sha(self.input / name)
                   for name in files if name.startswith("source/") and "/build-info/" not in name}
        installer.write(self.input / "source/build-info/application-sources.json", json.dumps(sources))
        record = {"schemaVersion": 2, "displayName": installer.DISPLAY_NAME, "executable": installer.EXECUTABLE,
                  "target": "x86_64-pc-windows-msvc", "profile": "legacy", "version": "0.1.0-beta.1",
                  "features": sorted(installer.portable.PROFILES["legacy"]["resolved"]),
                  "audioBackend": "FFmpeg native AAC; no FDK", "sourceDirty": False,
                  "sourceBaseCommit": "1" * 40, "channel": "beta", "releaseQualified": False,
                  "sourceFiles": len(sources), "sourceTreeSha256": installer.portable.snapshot_hash(sources),
                  "sha256": installer.sha(self.input / installer.EXECUTABLE)}
        installer.write(self.input / "BUILD.json", json.dumps(record))
        self.checksums()
        return record

    def checksums(self):
        entries = sorted(path for path in self.input.rglob("*") if path.is_file() and path.name != "SHA256SUMS.txt")
        installer.write(self.input / "SHA256SUMS.txt", "".join(
            f"{installer.sha(path)}  {path.relative_to(self.input).as_posix()}\n" for path in entries))

    def test_valid_contract_fixture_is_accepted_without_execution(self):
        _, record, hashes = installer.verify_package(self.input)
        self.assertEqual(record, self.record)
        self.assertEqual(hashes[installer.EXECUTABLE], self.record["sha256"])

    def test_changed_bytes_unhashed_extras_and_private_state_are_rejected(self):
        (self.input / "README.txt").write_text("changed", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "hash mismatch"):
            installer.verify_package(self.input)
        self.checksums()
        extra = self.input / "extra.txt"
        extra.write_text("unhashed", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "inventory"):
            installer.verify_package(self.input)
        extra.unlink()
        installer.write(self.input / "local/identity.dpapi", "private")
        self.checksums()
        with self.assertRaisesRegex(RuntimeError, "Private|user data"):
            installer.verify_package(self.input)

    def test_wrong_profile_dirty_source_and_stable_claim_are_rejected(self):
        for field, value in [("profile", "hap"), ("sourceDirty", True), ("channel", "stable"),
                             ("releaseQualified", True), ("version", "0.1.0"), ("displayName", "Old name")]:
            record = dict(self.record, **{field: value})
            installer.write(self.input / "BUILD.json", json.dumps(record))
            self.checksums()
            with self.assertRaises(RuntimeError, msg=field):
                installer.verify_package(self.input)

    def test_rehashing_source_file_does_not_hide_source_snapshot_mismatch(self):
        installer.write(self.input / "source/src/main.rs", "modified after build")
        self.checksums()
        with self.assertRaisesRegex(RuntimeError, "source snapshot"):
            installer.verify_package(self.input)

    def test_paths_reject_traversal_inno_expansion_and_windows_aliases(self):
        for name in ["../private", "/absolute", "C:/private", "source/../private", "source\\private",
                     "file.", "file ", "source/{#macro}.txt", "source/with\nnewline"]:
            with self.assertRaises(RuntimeError, msg=name):
                installer.relative_name(name)

    def test_outputs_and_test_roots_are_new_and_separate(self):
        production = self.root / "dist/setup.exe"
        self.assertEqual(installer.fresh_output(production, False), production)
        installer.write(production, "preserved")
        with self.assertRaisesRegex(RuntimeError, "preserved"):
            installer.fresh_output(production, False)
        with self.assertRaises(RuntimeError):
            installer.fresh_output(self.root / "dist/test-only.exe", True)
        with self.assertRaises(RuntimeError):
            installer.test_root(self.root / "work/install-test")
        target = self.root / "local/install-test"
        self.assertEqual(installer.test_root(target), target)
        target.mkdir(parents=True)
        with self.assertRaises(RuntimeError):
            installer.test_root(target)
        self.assertNotEqual(installer.APP_ID, installer.TEST_APP_ID)

    def test_template_has_no_automatic_launch_or_recursive_deletion(self):
        source = (ROOT / "scripts/installer/iPhoneAirMirror.iss").read_text(encoding="utf-8")
        for section in ("[Run]", "[UninstallRun]", "[InstallDelete]", "[UninstallDelete]", "[Registry]"):
            self.assertNotIn("\n" + section + "\n", source)
        for setting in ("PrivilegesRequired=lowest", "MinVersion=10.0.15063", "CloseApplications=no", "RestartApplications=no",
                        "CreateUninstallRegKey=no", 'Parameters: "--data-dir ""{localappdata}\\iPhone Air Mirror"""',
                        "SetErrorMode($8003)", "TestInstallDirectoryPascal", "SafeAncestors", "DLLGetLastError"):
            self.assertIn(setting, source)
        self.assertNotIn("DirectoryHasFiles", source)


if __name__ == "__main__":
    unittest.main()
