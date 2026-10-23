"""Build a fresh FFmpeg release-candidate directory or source-complete local ZIP.

No upload, receiver launch, sound test or mutation of existing packages. Legacy
pairing is the default; HAP remains an explicitly experimental profile. Every
package is a candidate: synthetic validation does not establish physical A/V.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import struct
import subprocess
import uuid
import zipfile

ROOT = Path(__file__).resolve().parent.parent
TARGET = "x86_64-pc-windows-msvc"
PROFILES = {
    "legacy": {"features": ("ffmpeg-aac", "display-update-experiment"),
               "resolved": {"ffmpeg-aac", "display-update-experiment", "rotation-advertisement-experiment"},
               "service": "iPhone Air Mirror", "directory": "iPhoneAirMirror-release-candidate"},
    "hap": {"features": ("ffmpeg-aac", "hap-pairing-experiment"),
            "resolved": {"ffmpeg-aac", "display-update-experiment", "rotation-advertisement-experiment", "hap-pairing-experiment"},
            "service": "iPhone Air Mirror HAP", "directory": "iPhoneAirMirror-hap-release-candidate"},
}
UPSTREAM_REPLY_SHA = "5a7c123bba599bfca553e815c2ff1fed947636999a1deb44c2fa640e86b2a90a"
PRIVATE_NAMES = {"identity.dpapi", "identity.key", "receiver.log"}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def run(args, *, cwd=ROOT, env=None, timeout=900):
    result = subprocess.run(args, cwd=cwd, env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=timeout,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    require(result.returncode == 0,
            f"{args[0]} failed ({result.returncode}):\n{result.stderr[-6000:]}\n{result.stdout[-3000:]}")
    return result.stdout


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8", newline="\n")


def reject_links(path):
    """Check unresolved ancestors too: resolve() alone would conceal a junction."""
    for item in (path, *path.parents):
        try:
            info = item.lstat()
        except FileNotFoundError:
            continue
        require(not stat.S_ISLNK(info.st_mode)
                and not getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT,
                f"Linked paths are not allowed in packages: {item}")


def copy(src, dest):
    reject_links(src)
    require(src.is_file(), f"Missing input: {src}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dest)


def output_path(value, *, directory=False):
    path = Path(os.path.abspath(value))
    reject_links(path)
    # Direct children only: an innocent new file must not modify an older
    # fallback's directory or write through a nested deployment path.
    parents = {ROOT / "dist"} if directory else {ROOT / "dist", ROOT / "work"}
    require(path.parent in parents, "Output must be a direct child of dist/ (ZIPs may also use work/)")
    require(not path.exists(), f"Existing outputs are preserved; choose a new name: {path}")
    require(directory or path.suffix.lower() == ".zip", "Archive output must end in .zip")
    return path


def included_source(name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name:
        return False
    return name in {"Cargo.toml", "Cargo.lock", "build.rs", "LICENSE.md", "NOTICE.md", "README.md",
                    ".gitattributes", ".gitignore", "vendor/PATCHES.md", "vendor/SHA256SUMS"} or name.startswith(
                        ("src/", "native/", "assets/", "scripts/", "tests/", "docs/", "vendor/playfair/"))


def source_snapshot():
    names = run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"]).split("\0")
    result = {}
    for name in sorted(set(names)):
        if not included_source(name):
            continue
        path = ROOT / name
        reject_links(path)
        # Tracked deletions are legitimate dirty-source changes. They must not
        # silently reappear in the archive from HEAD.
        if path.is_file():
            require(path.name.lower() not in PRIVATE_NAMES, "Private state in source selection")
            result[name] = sha(path)
    require(all(name in result for name in ("Cargo.toml", "Cargo.lock", "build.rs", "LICENSE.md", "NOTICE.md")),
            "Required application/source-license files are missing")
    return result


def snapshot_hash(snapshot):
    value = "\n".join(f"{name}:{digest}" for name, digest in sorted(snapshot.items()))
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def validate_features(profile, features):
    require(set(features) == PROFILES[profile]["resolved"],
            "Receiver features differ from the selected profile")


def verify_pe(path):
    with path.open("rb") as stream:
        header = stream.read(64)
        require(len(header) == 64 and header[:2] == b"MZ", "Missing PE DOS signature")
        offset = struct.unpack_from("<I", header, 0x3c)[0]
        require(64 <= offset <= path.stat().st_size - 6, "Invalid PE header offset")
        stream.seek(offset)
        require(stream.read(6) == b"PE\0\0\x64\x86", "Package executable is not x64 PE")


def build(profile, env):
    target_dir = ROOT / "work" / f"release-{profile}-target"
    reject_links(target_dir)
    settings = PROFILES[profile]
    feature_args = ["--no-default-features", "--features", ",".join(settings["features"])]
    events = [json.loads(line) for line in run([
        "cargo", "build", "--release", "--locked", "--offline", "--target", TARGET,
        *feature_args, "--target-dir", str(target_dir), "--message-format=json-render-diagnostics"], env=env).splitlines() if line.strip()]
    artifacts = [event for event in events if event.get("reason") == "compiler-artifact"
                 and event.get("target", {}).get("name") == "airmirror-lite" and event.get("executable")]
    require(len(artifacts) == 1, "Cargo did not report exactly one receiver executable")
    artifact = artifacts[0]
    validate_features(profile, artifact["features"])
    executable = Path(artifact["executable"])
    reject_links(executable)
    require(executable.resolve().is_relative_to(target_dir.resolve()), "Cargo executable escaped its dedicated target directory")
    verify_pe(executable)
    native_events = [event for event in events if event.get("reason") == "build-script-executed"
                     and event.get("package_id") == artifact["package_id"]]
    require(len(native_events) == 1, "Native provenance missing")
    native_path = Path(native_events[0]["out_dir"]) / "native-build.json"
    reject_links(native_path)
    native = json.loads(native_path.read_text(encoding="utf-8-sig"))
    require(native.get("target") == TARGET and native.get("aac", {}).get("backend") == "ffmpeg-native-aac",
            "Only x64 FFmpeg AAC receivers may be packaged")
    prefix = Path(env.get("AIRMIRROR_FFMPEG_PREFIX", ROOT / "work/ffmpeg-aac-msvc"))
    if not prefix.is_absolute():
        prefix = ROOT / prefix
    reject_links(prefix)
    run(["pwsh", "-NoProfile", "-NonInteractive", "-File", str(ROOT / "scripts/Verify-FFmpeg.ps1"), "-Prefix", str(prefix)], env=env)
    manifest = json.loads((prefix / "airmirror-ffmpeg.json").read_text(encoding="utf-8-sig"))
    require(native["aac"]["upstream"] == manifest, "FFmpeg prefix differs from the dependency linked into this receiver")
    metadata = json.loads(run(["cargo", "metadata", "--locked", "--offline", "--format-version", "1",
                              "--filter-platform", TARGET, *feature_args], env=env))
    return executable, native, prefix, metadata, feature_args


def redact_paths(value):
    serialized = json.dumps(value, indent=2)
    for original, replacement in [(str(ROOT), "PROJECT_ROOT"), (os.environ.get("USERPROFILE", ""), "HOME")]:
        if original:
            serialized = serialized.replace(original.replace("\\", "\\\\"), replacement)
            serialized = serialized.replace(original.replace("\\", "/"), replacement)
    return serialized + "\n"


def add_notices(package, prefix, metadata, scratch, env):
    for name in ("LICENSE.md", "NOTICE.md"):
        copy(ROOT / name, package / name)
    for source, name in [("vendor/playfair/LICENSE.md", "PlayFair-GPLv3.md"),
                         ("vendor/playfair/LGPL-2.1.txt", "LGPL-2.1.txt"),
                         ("vendor/playfair/REPLY_TABLE_NOTICE.txt", "REPLY_TABLE_NOTICE.txt"),
                         ("tests/fixtures/FAIRPLAY_NOTICE.txt", "FAIRPLAY_FIXTURE_NOTICE.txt"),
                         ("docs/THIRD_PARTY.md", "THIRD_PARTY.md"),
                         ("vendor/PATCHES.md", "VENDOR_PATCHES.md")]:
        copy(ROOT / source, package / "licenses" / name)
    for name in ("licenses/COPYING.LGPLv2.1", "licenses/LICENSE.md"):
        copy(prefix / name, package / "licenses/FFmpeg" / Path(name).name)
    metadata_file = scratch / "cargo-metadata.json"
    write(metadata_file, json.dumps(metadata))
    # Read the metadata from a file through a script parameter, not shell text.
    run(["pwsh", "-NoProfile", "-NonInteractive", "-File", str(ROOT / "scripts/Write-DependencyNotices.ps1"),
         "-MetadataFile", str(metadata_file), "-Destination", str(package / "licenses")], env=env)


def add_source(package, snapshot, prefix, native, env, feature_args):
    source = package / "source"
    for name, digest in snapshot.items():
        copy(ROOT / name, source / name)
        require(sha(source / name) == digest, "Source changed during package staging")
    require(not (source / "vendor/fdk-aac").exists(), "Unexpected FDK source in release package")
    require(sha(source / "vendor/playfair/upstream-reference/fairplay_playfair.c") == UPSTREAM_REPLY_SHA,
            "Pinned reply-table corresponding source mismatch")
    # Includes all locked build/proc-macro dependencies, not just runtime crates.
    run(["cargo", "vendor", "--locked", "--offline", str(source / "vendor/rust")], env=env)
    write(source / ".cargo/config.toml", '[source.crates-io]\nreplace-with = "vendored-sources"\n\n'
          '[source.vendored-sources]\ndirectory = "vendor/rust"\n')
    fresh_env = env.copy()
    fresh_env["CARGO_HOME"] = str(package.parent / "empty-cargo-home")
    run(["cargo", "metadata", "--locked", "--offline", "--format-version", "1",
         "--filter-platform", TARGET, *feature_args], cwd=source, env=fresh_env)
    pins = json.loads((ROOT / "scripts/ffmpeg-pins.json").read_text(encoding="utf-8"))
    archive = "ffmpeg-" + pins["source"]["version"] + ".tar.xz"
    for name, digest in [(archive, pins["source"]["sha256"]),
                         (archive + ".asc", pins["source"]["signatureSha256"]),
                         ("ffmpeg-devel.asc", pins["source"]["keySha256"])]:
        path = ROOT / "work/ffmpeg-deps" / name
        require(sha(path) == digest.lower(), "Pinned FFmpeg source/signature mismatch")
        copy(path, source / "vendor/ffmpeg" / name)
    for name in ("airmirror-ffmpeg.json", "config/config.h", "config/config_components.h"):
        copy(prefix / name, source / "build-info/ffmpeg" / name)
    write(source / "build-info/native-build.json", redact_paths(native))
    write(source / "build-info/rust-toolchain.txt", run(["rustc", "--version", "--verbose"]))
    recipe = (ROOT / "scripts/share/REBUILD.txt").read_text(encoding="utf-8")
    write(source / "REBUILD.txt", recipe.replace("@FEATURES@", feature_args[-1]))


def package_files(package):
    files = []
    for path in package.rglob("*"):
        reject_links(path)
        if path.is_file():
            require(path.name.lower() not in PRIVATE_NAMES, "Private state leaked into package")
            relative = path.relative_to(package)
            require(relative.parts[0] not in {".git", "work", "target"}, "Development state in package root")
            if relative.parts[0] == "local":
                require(relative.as_posix() == "local/display-aspect.txt", "Unexpected local user data")
            files.append(path)
    return sorted(files)


def archive_package(package, output):
    reject_links(output)
    files = package_files(package)
    temporary = output.with_name(output.name + ".partial-" + uuid.uuid4().hex[:12])
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        # Clamp ZIP metadata only; vendored Cargo source epochs may predate 1980.
        with zipfile.ZipFile(temporary, "x", zipfile.ZIP_DEFLATED, compresslevel=6, strict_timestamps=False) as archive:
            for path in files:
                archive.write(path, "iPhoneAirMirror/" + path.relative_to(package).as_posix())
        with zipfile.ZipFile(temporary) as archive:
            require(archive.testzip() is None, "Archive CRC verification failed")
            require(len(archive.namelist()) == len(files) == len(set(archive.namelist())), "Duplicate/missing ZIP entry")
            for path in files:
                with archive.open("iPhoneAirMirror/" + path.relative_to(package).as_posix()) as stream:
                    require(hashlib.file_digest(stream, "sha256").hexdigest() == sha(path), "Archive content mismatch")
        require(not output.exists(), "Output appeared during packaging; existing archives are preserved")
        temporary.rename(output)  # On Windows rename fails if the target exists.
    finally:
        if temporary.exists():
            temporary.unlink()
    return len(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=PROFILES, default="legacy")
    parser.add_argument("--output", type=Path, help="New source-complete ZIP under dist/ or work/")
    parser.add_argument("--directory", type=Path, help="New candidate directory directly under dist/")
    args = parser.parse_args()
    settings = PROFILES[args.profile]
    output = output_path(args.output) if args.output else None
    destination = output_path(args.directory, directory=True) if args.directory else None
    if output is None and destination is None:
        destination = output_path(ROOT / "dist" / settings["directory"], directory=True)
    if output:
        require(not output.with_suffix(".verification.json").exists(), "Existing ZIP verification reports are preserved")
    env = os.environ.copy()
    env["CARGO_HOME"] = str(ROOT / "work/cargo")
    before = source_snapshot()
    executable, native, prefix, metadata, feature_args = build(args.profile, env)
    require(source_snapshot() == before, "Source changed during build; retry after edits finish")
    executable_hash = sha(executable)
    scratch = ROOT / "work" / ("release-package-" + uuid.uuid4().hex[:12])
    reject_links(scratch)
    package = scratch / "iPhoneAirMirror"
    package.mkdir(parents=True, exist_ok=False)
    copy(executable, package / "iPhoneAirMirror.exe")
    copy(ROOT / "scripts/share/Start-iPhoneAirMirror.cmd", package / "Start-iPhoneAirMirror.cmd")
    description = ("FFmpeg AAC release candidate with legacy pairing" if args.profile == "legacy"
                   else "Experimental verified HAP pairing candidate; physical HAP audio/A/V is unverified")
    readme = (ROOT / "scripts/share/README.txt").read_text(encoding="utf-8")
    write(package / "README.txt", readme.replace("@SERVICE_NAME@", settings["service"]).replace("@PROFILE_DESCRIPTION@", description))
    write(package / "local/display-aspect.txt", "994 2160\n")
    add_notices(package, prefix, metadata, scratch, env)
    add_source(package, before, prefix, native, env, feature_args)
    require(source_snapshot() == before and sha(executable) == executable_hash,
            "Source or built executable changed during packaging")
    app_metadata = next(p for p in metadata["packages"] if p["id"] == metadata["resolve"]["root"])
    record = {"schemaVersion": 2, "package": description, "profile": args.profile,
              "displayName": "iPhone Air Mirror", "version": app_metadata["version"], "channel": "beta",
              "sha256": executable_hash, "executable": "iPhoneAirMirror.exe", "target": TARGET,
              "createdUtc": datetime.now(timezone.utc).isoformat(),
              "features": sorted(settings["resolved"]), "requestedFeatures": list(settings["features"]),
              "sourceBaseCommit": run(["git", "rev-parse", "HEAD"]).strip(),
              "sourceDirty": bool(run(["git", "status", "--porcelain"]).strip()),
              "sourceTreeSha256": snapshot_hash(before), "sourceFiles": len(before),
              "sourceHashScope": "Included application source: sorted relative-name:SHA256 lines, UTF-8, LF, no final newline",
              "sourceCorrespondence": "Locked offline build; selected source hashes unchanged before build through package staging",
              "sourceLicense": "GPL-3.0-only; imported components retain their notices",
              "audioBackend": "FFmpeg native AAC; no FDK", "releaseQualified": False,
              "physicalAcceptance": "See source/docs/STATE.md; build, synthetic and GPU checks do not establish iPhone audio/sync acceptance",
              "sourceMaterials": "source/REBUILD.txt; complete pinned FFmpeg and locked Rust sources included",
              "userData": "No identity, logs or captures; only a newly generated 994:2160 display preset"}
    write(package / "BUILD.json", json.dumps(record, indent=2) + "\n")
    write(package / "source/build-info/application-sources.json", json.dumps(before, indent=2) + "\n")
    files = package_files(package)
    write(package / "SHA256SUMS.txt", "".join(f"{sha(path)}  {path.relative_to(package).as_posix()}\n" for path in files))
    # Provision this fresh owned copy only. This mode opens no UI, audio or listener.
    run([str(package / "iPhoneAirMirror.exe"), "--provision-worker"], timeout=15)
    report = {"profile": args.profile, "executableSha256": executable_hash, "stage": str(package),
              "sourceTreeSha256": record["sourceTreeSha256"], "releaseQualified": False,
              "checks": ["fresh locked offline profile build", "x64 PE and FFmpeg provenance",
                         "unchanged source snapshot", "offline vendored resolution with empty Cargo home",
                         "pinned dependency sources", "no identities/logs", "worker permission provisioned"]}
    if output:
        report["files"] = archive_package(package, output)
        report.update(zip=str(output), zipSha256=sha(output), zipBytes=output.stat().st_size)
        report["checks"].append("all ZIP entries CRC and SHA-256")
    if destination:
        reject_links(destination)
        require(not destination.exists(), "Candidate destination appeared during packaging")
        destination.parent.mkdir(parents=True, exist_ok=True)
        package.rename(destination)
        report["directory"] = str(destination)
        report["stage"] = str(destination)
    if output:
        write(output.with_suffix(".verification.json"), json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
