"""Wrap a verified, source-complete portable beta in a per-user Inno installer.

Compilation only: never installs, launches the receiver, changes Windows settings
or signs/publishes anything. Existing outputs and portable packages are preserved.
An explicit test-only build can install app/data solely beneath workspace local/.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tomllib
import uuid

ROOT = Path(__file__).resolve().parent.parent
ISCC = Path("C:/Program Files (x86)/Inno Setup 6/ISCC.exe")
APP_ID = "{9CE0F172-BBDD-483F-8A10-E258BBE04549}"
TEST_APP_ID = "{11B56199-8EA6-4DA8-98B6-EDB5DDABF737}"
DISPLAY_NAME, EXECUTABLE = "iPhone Air Mirror", "iPhoneAirMirror.exe"
spec = importlib.util.spec_from_file_location("portable_package", ROOT / "scripts/Package-Share.py")
portable = importlib.util.module_from_spec(spec)
spec.loader.exec_module(portable)
require, sha, reject_links = portable.require, portable.sha, portable.reject_links
MAX_FILES, MAX_MANIFEST_BYTES = 20000, 4 * 1024 * 1024


def relative_name(value):
    require(isinstance(value, str) and value and len(value) <= 1024, "Invalid package path")
    path = PurePosixPath(value)
    require(not path.is_absolute() and path.as_posix() == value
            and all(part not in (".", "..") and not part.endswith((" ", ".")) for part in path.parts)
            and not any(c in value for c in '\\<>:"|?*{}') and not any(ord(c) < 32 for c in value),
            "Unsafe package path")
    return path


def read_json(path):
    with path.open("rb") as handle:
        data = handle.read(MAX_MANIFEST_BYTES + 1)
    require(len(data) <= MAX_MANIFEST_BYTES, f"Oversized manifest: {path.name}")
    value = json.loads(data.decode("utf-8-sig"))
    require(isinstance(value, dict), f"Expected an object in {path.name}")
    return value


def verify_package(directory):
    """Treat the entire input as immutable and reject omitted/unhashed extras."""
    directory = Path(os.path.abspath(directory))
    reject_links(directory)
    require(directory.is_dir(), "Portable input must be an existing directory")
    files = portable.package_files(directory)
    require(1 < len(files) <= MAX_FILES, "Portable file count exceeds its budget")
    actual = {path.relative_to(directory).as_posix(): path for path in files}
    require(len({name.casefold() for name in actual}) == len(actual), "Case-colliding package paths")
    for name in actual:
        relative_name(name)
    manifest = directory / "SHA256SUMS.txt"
    require(manifest.is_file() and manifest.stat().st_size <= MAX_MANIFEST_BYTES, "Missing/oversized portable checksums")
    hashes = {}
    for line in manifest.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]{64})  (.+)", line)
        require(match is not None, "Malformed portable checksum entry")
        digest, name = match.groups()
        relative_name(name)
        require(name not in hashes, "Duplicate portable checksum entry")
        hashes[name] = digest.lower()
    require(set(hashes) == set(actual) - {"SHA256SUMS.txt"}, "Portable checksum inventory is incomplete or contains extras")
    for name, digest in hashes.items():
        require(sha(actual[name]) == digest, f"Portable hash mismatch: {name}")
    required = {EXECUTABLE, "BUILD.json", "LICENSE.md", "NOTICE.md", "README.txt",
                "Start-iPhoneAirMirror.cmd", "local/display-aspect.txt", "source/Cargo.toml",
                "source/Cargo.lock", "source/.cargo/config.toml", "source/assets/app-icon.ico",
                "source/build.rs", "source/src/main.rs", "source/native/windows.cpp",
                "source/native/codec_ffmpeg.cpp", "source/vendor/playfair/LICENSE.md",
                "source/REBUILD.txt", "source/build-info/application-sources.json",
                "source/build-info/native-build.json", "licenses/RUST-DEPENDENCIES.json"}
    require(required <= hashes.keys(), "Portable package lacks required source/license/build material")
    require(any(name.startswith("source/vendor/rust/") for name in hashes)
            and any(name.startswith("source/vendor/ffmpeg/") and name.endswith(".tar.xz") for name in hashes),
            "Complete locked Rust and FFmpeg sources are required")
    record = read_json(directory / "BUILD.json")
    require(record.get("schemaVersion") == 2 and record.get("displayName") == DISPLAY_NAME
            and record.get("executable") == EXECUTABLE and record.get("target") == "x86_64-pc-windows-msvc",
            "Portable identity/target does not match this installer")
    require(record.get("profile") == "legacy" and set(record.get("features", [])) == portable.PROFILES["legacy"]["resolved"]
            and record.get("audioBackend") == "FFmpeg native AAC; no FDK", "Installer requires the legacy FFmpeg profile")
    require(record.get("sourceDirty") is False and re.fullmatch(r"[0-9a-f]{40}", record.get("sourceBaseCommit", "")),
            "Installer requires a clean committed portable source snapshot")
    require(record.get("channel") == "beta" and record.get("releaseQualified") is False,
            "This packaging path is for an explicitly unqualified beta")
    version = record.get("version", "")
    require(re.fullmatch(r"(?:0|[1-9][0-9]{0,3})\.(?:0|[1-9][0-9]{0,3})\.(?:0|[1-9][0-9]{0,3})-beta\.[1-9][0-9]{0,3}", version),
            "Expected a bounded x.y.z-beta.n version")
    require(record.get("sha256", "").lower() == hashes[EXECUTABLE], "Executable differs from BUILD.json")
    portable.verify_pe(directory / EXECUTABLE)
    source = read_json(directory / "source/build-info/application-sources.json")
    require(type(record.get("sourceFiles")) is int and record["sourceFiles"] == len(source), "Source inventory count mismatch")
    for name, digest in source.items():
        relative_name(name)
        require(isinstance(digest, str) and re.fullmatch(r"[0-9a-f]{64}", digest)
                and hashes.get("source/" + name) == digest, "Application source snapshot mismatch")
    require(portable.snapshot_hash(source) == record.get("sourceTreeSha256"), "Source tree identity mismatch")
    cargo = tomllib.loads((directory / "source/Cargo.toml").read_text(encoding="utf-8"))
    require(cargo.get("package", {}).get("version") == version, "Cargo version differs from BUILD.json")
    native = read_json(directory / "source/build-info/native-build.json")
    require(native.get("target") == record["target"] and native.get("aac", {}).get("backend") == "ffmpeg-native-aac",
            "Native backend provenance mismatch")
    return directory, record, hashes


def fresh_output(value, test_only):
    path = Path(os.path.abspath(value))
    reject_links(path)
    require(path.parent == ROOT / ("work" if test_only else "dist") and path.suffix.lower() == ".exe",
            "Production output must be directly under dist/; test-only output directly under work/")
    require(not path.exists() and not path.with_suffix(".installer.json").exists(), "Existing installer outputs are preserved")
    require(not test_only or "test-only" in path.name.lower(), "Test installer filename must include test-only")
    return path


def test_root(value):
    if value is None:
        return None
    path = Path(os.path.abspath(value))
    reject_links(path)
    require(path.parent == ROOT / "local" and not path.exists(), "Test install root must be a fresh direct child of workspace local/")
    require(not any(c in str(path) for c in '"{}\r\n'), "Unsupported test install path")
    return path


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def stage_payload(directory, hashes, scratch, app_id):
    payload = scratch / "payload"
    relocated = {"README.txt", "SHA256SUMS.txt", "Start-iPhoneAirMirror.cmd", "local/display-aspect.txt"}
    for name in [*hashes, "SHA256SUMS.txt"]:
        destination = payload / ("source/build-info/portable-origin/" + name if name in relocated else name)
        portable.copy(directory / name, destination)
        require(sha(destination) == (hashes[name] if name in hashes else sha(directory / name)), "Input changed during installer staging")
    for name in ("iPhoneAirMirror.iss", "README.txt"):
        portable.copy(ROOT / "scripts/installer" / name, payload / "source/build-info/installer" / name)
    portable.copy(ROOT / "scripts/Package-Installer.py", payload / "source/build-info/installer/Package-Installer.py")
    portable.copy(ROOT / "scripts/installer/README.txt", payload / "README.txt")
    write(payload / "installation-owner.txt", app_id + "\n")
    installed = {path.relative_to(payload).as_posix(): sha(path) for path in portable.package_files(payload)}
    write(payload / "installed-SHA256SUMS.txt", "".join(f"{digest}  {name}\n" for name, digest in sorted(installed.items())))
    return payload


def compiler_files(payload, scratch, record, isolated_root):
    require(not any(c in str(scratch) for c in '"{}\r\n'), "Unsupported compiler workspace path")
    app_id = TEST_APP_ID if isolated_root else APP_ID
    config = {"InstallerAppId": "{" + app_id, "AppVersion": record["version"],
              "NumericVersion": record["version"].split("-", 1)[0] + ".0",
              "PayloadDirectory": str(payload), "CompilerOutputDirectory": str(scratch / "output"),
              "CompilerOutputName": "installer-test-only" if isolated_root else "installer",
              "InstallDirectory": str(isolated_root / "app") if isolated_root else r"{localappdata}\Programs\iPhone Air Mirror",
              "DataDirectory": str(isolated_root / "data") if isolated_root else r"{localappdata}\iPhone Air Mirror",
              "OwnerMarkerSha256": sha(payload / "installation-owner.txt")}
    if isolated_root:
        config["TestInstallDirectoryPascal"] = str(isolated_root / "app").replace("'", "''")
        config["TestDataDirectoryPascal"] = str(isolated_root / "data").replace("'", "''")
    write(scratch / "package-config.iss", "\n".join(f'#define {key} "{value}"' for key, value in config.items())
          + f"\n#define TestOnly {1 if isolated_root else 0}\n")
    files = sorted(portable.package_files(payload), key=lambda path: (path.name != EXECUTABLE, str(path)))
    entries = []
    for path in files:
        relative = path.relative_to(payload)
        parent = str(relative.parent)
        destination = "{app}" if parent == "." else "{app}\\" + parent
        callback = "; AfterInstall: ProvisionWorker" if relative.as_posix() == EXECUTABLE else ""
        entries.append(f'Source: "{path}"; DestDir: "{destination}"; Flags: ignoreversion; Hash: "{sha(path)}"{callback}')
    preset = payload / "source/build-info/portable-origin/local/display-aspect.txt"
    entries.append(f'Source: "{preset}"; DestDir: "{{#DataDirectory}}"; Flags: onlyifdoesntexist uninsneveruninstall; Hash: "{sha(preset)}"')
    write(scratch / "package-files.iss", "\n".join(entries) + "\n")
    portable.copy(ROOT / "scripts/installer/iPhoneAirMirror.iss", scratch / "installer.iss")
    return scratch / "installer.iss", scratch / "output" / (config["CompilerOutputName"] + ".exe")


def signature_status(path, scratch):
    probe = scratch / "signature.ps1"
    write(probe, "param([string]$ArtifactPath)\n$ErrorActionPreference = 'Stop'\n"
          "$signature = Get-AuthenticodeSignature -LiteralPath $ArtifactPath\n"
          "@{status=$signature.Status.ToString(); signed=($null -ne $signature.SignerCertificate)} | ConvertTo-Json -Compress\n")
    return json.loads(portable.run(["pwsh", "-NoProfile", "-NonInteractive", "-File", str(probe), "-ArtifactPath", str(path)]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--portable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--iscc", type=Path, default=ISCC)
    parser.add_argument("--test-install-root", type=Path, help="Compile a separate workspace-only lifecycle-test installer")
    args = parser.parse_args()
    isolated = test_root(args.test_install_root)
    output = fresh_output(args.output, isolated is not None)
    directory, record, hashes = verify_package(args.portable)
    reject_links(args.iscc)
    require(args.iscc.is_file(), "Install Inno Setup 6.7.1 or newer before compiling")
    scratch = ROOT / "work" / ("installer-package-" + uuid.uuid4().hex[:12])
    reject_links(scratch)
    scratch.mkdir(parents=True, exist_ok=False)
    app_id = TEST_APP_ID if isolated else APP_ID
    payload = stage_payload(directory, hashes, scratch, app_id)
    script, compiled = compiler_files(payload, scratch, record, isolated)
    result = subprocess.run([str(args.iscc), str(script)], stdin=subprocess.DEVNULL, capture_output=True,
                            text=True, encoding="utf-8", errors="replace", timeout=900,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    write(scratch / "compiler.log", result.stdout + result.stderr)
    require(result.returncode == 0 and compiled.is_file(), f"Installer compilation failed; see {scratch / 'compiler.log'}")
    _, after_record, after_hashes = verify_package(directory)
    require(after_record == record and after_hashes == hashes, "Portable input changed during compilation")
    signature = signature_status(compiled, scratch)
    require(signature["status"] == "NotSigned" and not signature["signed"], "Unexpected signing state; this pipeline must not sign installers")
    digest = sha(compiled)
    receipt = {"schemaVersion": 1, "displayName": DISPLAY_NAME, "version": record["version"],
               "channel": "beta", "testOnly": isolated is not None, "appId": app_id,
               "createdUtc": datetime.now(timezone.utc).isoformat(), "sha256": digest,
               "bytes": compiled.stat().st_size, "signature": signature,
               "portableExecutableSha256": hashes[EXECUTABLE], "portableManifestSha256": hashes["BUILD.json"],
               "portableSourceCommit": record["sourceBaseCommit"], "portableSourceTreeSha256": record["sourceTreeSha256"],
               "compilerSha256": sha(args.iscc), "installedFilesManifestSha256": sha(payload / "installed-SHA256SUMS.txt"),
               "installerScriptSha256": sha(ROOT / "scripts/installer/iPhoneAirMirror.iss"),
               "checks": ["exhaustive portable checksums", "clean committed source snapshot", "x64 FFmpeg legacy beta identity",
                          "same executable bytes", "Inno per-file compile hashes", "unchanged portable input", "actual Authenticode status"],
               "installationTested": False, "releaseQualified": False,
               "installationPolicy": "Per-user; no auto-launch, app auto-close, network/security changes or recursive data deletion; user preset retained",
               "workspaceEvidence": scratch.relative_to(ROOT).as_posix()}
    if isolated:
        receipt["testInstallRoot"] = str(isolated)
        receipt["testBoundary"] = "Separate AppId; no shortcuts or uninstall registry entry; workspace app/data roots only"
    output.parent.mkdir(parents=True, exist_ok=True)
    require(not output.exists(), "Installer output appeared while compiling")
    compiled.rename(output)
    with output.with_suffix(".installer.json").open("x", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
