"""Package the verified FFmpeg candidate with matching source for a friend.

No upload, receiver stop, UI automation, sound test or fallback mutation.
Run after committing packaging changes for clean source provenance.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import uuid
import zipfile

ROOT = Path(__file__).resolve().parent.parent
FEATURES = "ffmpeg-aac,display-update-experiment"
UPSTREAM_REPLY_SHA = "5a7c123bba599bfca553e815c2ff1fed947636999a1deb44c2fa640e86b2a90a"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def run(args, *, cwd=ROOT, env=None):
    result = subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=180)
    require(result.returncode == 0,
            f"{args[0]} failed ({result.returncode}):\n{result.stderr[-6000:]}\n{result.stdout[-3000:]}")
    return result.stdout


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8", newline="\n")


def copy(src, dest):
    require(src.is_file() and not src.is_symlink(), f"Missing or linked input: {src}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dest)


def included_source(name):
    return name in {"Cargo.toml", "Cargo.lock", "build.rs", "LICENSE.md", "NOTICE.md",
                    "README.md", ".gitattributes", ".gitignore", "vendor/PATCHES.md",
                    "vendor/SHA256SUMS", "docs/THIRD_PARTY.md", "docs/DECODER_EXPERIMENT.md",
                    "docs/ROTATION_REGRESSION.md"} or name.startswith(
                        ("src/", "native/", "assets/", "scripts/", "tests/", "vendor/playfair/"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    require(output.is_relative_to(ROOT / "dist") or output.is_relative_to(ROOT / "work"),
            "ZIP output must remain under this workspace's dist/ or work/")
    require(not output.exists(), "Choose a fresh output filename; existing archives are preserved")
    candidate = ROOT / "dist/AirMirrorLite-ffmpeg-candidate"
    exe = candidate / "AirMirrorLite.exe"
    candidate_record = json.loads((candidate / "BUILD.json").read_text(encoding="utf-8-sig"))
    exe_hash = sha(exe)
    require(exe_hash == candidate_record["sha256"].lower(), "Candidate manifest/hash mismatch")
    require(set(candidate_record["experimentalFeatures"]) == set(FEATURES.split(",")),
            "This packaging path is only for the explicit FFmpeg/display candidate")

    env = os.environ.copy()
    env["CARGO_HOME"] = str(ROOT / "work/cargo")
    events = [json.loads(line) for line in run([
        "cargo", "build", "--release", "--locked", "--offline", "--features", FEATURES,
        "--target-dir", str(ROOT / "work/ffmpeg-receiver-target"),
        "--message-format=json-render-diagnostics"], env=env).splitlines() if line.strip()]
    artifacts = [event for event in events if event.get("reason") == "compiler-artifact"
                 and event.get("target", {}).get("name") == "airmirror-lite" and event.get("executable")]
    require(len(artifacts) == 1 and sha(Path(artifacts[0]["executable"])) == exe_hash,
            "Current source does not reproduce the exact tested candidate; validate a new candidate first")
    require("decoder-lab" not in artifacts[0]["features"], "Decode-only lab is not a receiver")
    native_events = [event for event in events if event.get("reason") == "build-script-executed"
                     and event.get("package_id") == artifacts[0]["package_id"]]
    require(len(native_events) == 1, "Native provenance missing")
    native_file = Path(native_events[0]["out_dir"]) / "native-build.json"
    native = json.loads(native_file.read_text(encoding="utf-8-sig"))
    require(native["aac"]["backend"].startswith("ffmpeg-native-aac"), "FDK is forbidden in this ZIP")

    scratch = ROOT / "work" / ("share-package-" + uuid.uuid4().hex[:12])
    package = scratch / "AirMirrorLite"
    source = package / "source"
    source.mkdir(parents=True)
    for name in sorted(set(run(["git", "ls-files", "--cached", "--others", "--exclude-standard"]).splitlines())):
        if included_source(name):
            copy(ROOT / name, source / name)
    require(not (source / "vendor/fdk-aac").exists(), "Unexpected FDK source in share tree")
    for name in ["LICENSE.md", "NOTICE.md"]:
        copy(ROOT / name, package / name)
    copy(exe, package / "AirMirrorLite.exe")
    copy(ROOT / "scripts/share/Start-AirMirrorLite.cmd", package / "Start-AirMirrorLite.cmd")
    copy(ROOT / "scripts/share/README.txt", package / "README.txt")
    copy(ROOT / "scripts/share/REBUILD.txt", source / "REBUILD.txt")
    write(package / "local/display-aspect.txt", "994 2160\n")  # Public tested preset, not copied user data.

    # Include full locked sources, including build/proc-macro dependencies.
    run(["cargo", "vendor", "--locked", "--offline", str(source / "vendor/rust")], env=env)
    write(source / ".cargo/config.toml", '[source.crates-io]\nreplace-with = "vendored-sources"\n\n'
          '[source.vendored-sources]\ndirectory = "vendor/rust"\n')
    metadata = json.loads(run(["cargo", "metadata", "--locked", "--offline", "--format-version", "1",
                              "--filter-platform", "x86_64-pc-windows-msvc", "--features", FEATURES], env=env))
    inventory = [{"name": p["name"], "version": p["version"], "license": p["license"], "source": p["source"]}
                 for p in metadata["packages"] if p["source"]]
    write(package / "licenses/RUST-SOURCE-INVENTORY.json", json.dumps(inventory, indent=2) + "\n")
    fresh_env = env.copy()
    fresh_env["CARGO_HOME"] = str(scratch / "empty-cargo-home")
    run(["cargo", "metadata", "--locked", "--offline", "--format-version", "1",
         "--filter-platform", "x86_64-pc-windows-msvc", "--features", FEATURES], cwd=source, env=fresh_env)

    pins = json.loads((ROOT / "scripts/ffmpeg-pins.json").read_text())
    archive = "ffmpeg-" + pins["source"]["version"] + ".tar.xz"
    for name, digest in [(archive, pins["source"]["sha256"]),
                         (archive + ".asc", pins["source"]["signatureSha256"]),
                         ("ffmpeg-devel.asc", pins["source"]["keySha256"])]:
        path = ROOT / "work/ffmpeg-deps" / name
        require(sha(path) == digest.lower(), "Pinned FFmpeg source/signature mismatch")
        copy(path, source / "vendor/ffmpeg" / name)
    reply = ROOT / "work/share-upstream/fairplay_playfair.c"
    require(sha(reply) == UPSTREAM_REPLY_SHA, "Pinned reply-table source mismatch")
    copy(reply, source / "vendor/playfair/upstream-reference/fairplay_playfair.c")
    # The reference is not compiled; production uses the checked extracted table.
    write(source / "vendor/playfair/upstream-reference/README.txt",
          "Unmodified corresponding source for replies.bin. LGPL-2.1-or-later.\n"
          "Origin: UxPlay 15a6b87172eca38b0df4e615fbf1bba108734dde/lib/fairplay_playfair.c\n"
          "This reference is not part of the build's native source glob.\n")

    for item in (candidate / "licenses").rglob("*"):
        if item.is_file():
            relative = item.relative_to(candidate / "licenses")
            require("fdk" not in str(relative).lower(), "Stale FDK notice suggests wrong package")
            copy(item, package / "licenses" / relative)
    copy(ROOT / "docs/THIRD_PARTY.md", package / "licenses/THIRD_PARTY.md")
    copy(ROOT / "vendor/PATCHES.md", package / "licenses/VENDOR_PATCHES.md")
    for name in ["airmirror-ffmpeg.json", "config/config.h", "config/config_components.h"]:
        copy(ROOT / "work/ffmpeg-aac-msvc" / name, source / "build-info/ffmpeg" / name)
    serialized = json.dumps(native, indent=2)
    for original, replacement in [(str(ROOT), "PROJECT_ROOT"), (os.environ["USERPROFILE"], "HOME")]:
        serialized = serialized.replace(original.replace("\\", "\\\\"), replacement)
        serialized = serialized.replace(original.replace("\\", "/"), replacement)
    write(source / "build-info/native-build.json", serialized + "\n")
    write(source / "build-info/rust-toolchain.txt", run(["rustc", "--version", "--verbose"]))

    record = {"package": "AirMirrorLite Windows x64 friend test", "sha256": exe_hash,
              "createdUtc": datetime.now(timezone.utc).isoformat(),
              "features": FEATURES.split(","), "executableSourceCommit": candidate_record["sourceCommit"],
              "packagingCommit": run(["git", "rev-parse", "HEAD"]).strip(),
              "packagingSourceDirty": bool(run(["git", "status", "--porcelain"]).strip()),
              "sourceCorrespondence": "Offline Cargo freshness build equals the tested candidate hash",
              "sourceLicense": "GPL-3.0-only; imported components retain their notices",
              "audioBackend": "FFmpeg native AAC; no FDK", "testBuild": True,
              "physicalAcceptance": "Latest Photos landscape and FFmpeg A/V confirmation remain pending",
              "sourceMaterials": "source/REBUILD.txt; complete pinned FFmpeg and locked Rust sources included",
              "userData": "No identity, logs or captures; only a newly generated 994:2160 display preset"}
    write(package / "BUILD.json", json.dumps(record, indent=2) + "\n")
    files = sorted(p for p in package.rglob("*") if p.is_file())
    require(sorted(p.name for p in (package / "local").iterdir()) == ["display-aspect.txt"], "User data leak")
    require(not any(p.name in {"identity.dpapi", "identity.key", "receiver.log"} for p in files), "Private state leak")
    write(package / "SHA256SUMS.txt", "".join(f"{sha(p)}  {p.relative_to(package).as_posix()}\n" for p in files))
    files = sorted(p for p in package.rglob("*") if p.is_file())
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".partial-" + uuid.uuid4().hex[:12])
    # Cargo preserves source epochs older than ZIP's 1980 lower bound. Clamp
    # archive metadata only; never modify the vendored source contents.
    with zipfile.ZipFile(temporary, "x", zipfile.ZIP_DEFLATED, compresslevel=6, strict_timestamps=False) as z:
        for path in files:
            z.write(path, "AirMirrorLite/" + path.relative_to(package).as_posix())
    with zipfile.ZipFile(temporary) as z:
        require(z.testzip() is None, "Archive CRC verification failed")
        require(len(z.namelist()) == len(files) == len(set(z.namelist())), "Duplicate/missing ZIP entry")
        for path in files:
            content = z.read("AirMirrorLite/" + path.relative_to(package).as_posix())
            require(hashlib.sha256(content).hexdigest() == sha(path), "Archive content mismatch")
    # Publish the final local filename only after complete verification.
    temporary.rename(output)
    report = {"zip": str(output), "zipSha256": sha(output), "zipBytes": output.stat().st_size,
              "files": len(files), "stage": str(package), "executableSha256": exe_hash,
              "checks": ["matching build hash", "offline vendored source resolution with empty Cargo home",
                         "pinned source hashes", "no identity/log files", "all ZIP entries CRC and SHA-256"]}
    write(output.with_suffix(".verification.json"), json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
