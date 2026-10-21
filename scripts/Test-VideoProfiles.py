"""Silent real /info wire checks; only processes/listeners created by this test."""
import argparse
import hashlib
import json
import pathlib
import plistlib
import socket
import subprocess
import time

root = pathlib.Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--exe", type=pathlib.Path, default=root / "target/release/airmirror-lite.exe")
parser.add_argument("--latency-ms", type=int, default=80)
parser.add_argument("--fps", type=int, choices=(30, 60), default=60)
parser.add_argument("--expect-rotation", action="store_true", help="Check the opt-in rotation advertisement experiment")
parser.add_argument("--display-width", type=int)
parser.add_argument("--display-aspect", type=int, nargs=2, metavar=("WIDTH", "HEIGHT"))
args = parser.parse_args()
if not 60 <= args.latency_ms <= 250:
    parser.error("--latency-ms must be 60..250")
if args.display_width is not None and (not 48 <= args.display_width <= 1920 or args.display_width % 2):
    parser.error("--display-width must be even and 48..1920 to test all three profiles")
if args.display_aspect and not all(48 <= value <= 3840 for value in args.display_aspect):
    parser.error("--display-aspect dimensions must be 48..3840")
data = root / "local/profile-test"
data.mkdir(parents=True, exist_ok=True)
result = data / "result.json"
result.unlink(missing_ok=True)
aspect_fixture = data / "display-aspect.txt"
aspect_fixture.unlink(missing_ok=True)
if args.display_aspect:
    aspect_fixture.write_text("{} {}\n".format(*args.display_aspect), encoding="ascii")
exe = args.exe.resolve(strict=True)
subprocess.run([str(exe), "--provision-worker"], check=True, timeout=10,
               stdin=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
executable_hash = hashlib.sha256(exe.read_bytes()).hexdigest().upper()
checked = []


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


for height, width, high in [(1080, 1920, False), (1440, 2560, True), (2160, 3840, True)]:
    fps = args.fps
    extra = []
    if args.display_width is not None:
        width = args.display_width
        extra = ["--display-width", str(width)]
    elif args.display_aspect:
        reference_w, reference_h = args.display_aspect
        width = ((reference_w * height + reference_h) // (2 * reference_h)) * 2
    with (data / f"{height}-out.txt").open("w") as out, (data / f"{height}-err.txt").open("w") as err:
        process = subprocess.Popen([str(exe), "--hidden", "--no-discovery", "--bind", "127.0.0.1",
                                    "--port", "17003", "--seconds", "3", "--resolution", str(height),
                                    "--latency-ms", str(args.latency_ms),
                                    "--fps", str(fps),
                                    "--data-dir", str(data)] + extra, stdin=subprocess.DEVNULL,
                                   stdout=out, stderr=err, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + 8
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f"{height} receiver exited; inspect {data}")
                try:
                    connection = socket.create_connection(("127.0.0.1", 17003), timeout=.5)
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(.05)
            with connection:
                connection.settimeout(2)
                connection.sendall(b"GET /info RTSP/1.0\r\nCSeq: 1\r\n\r\n")
                stream = connection.makefile("rb")
                require(b"200 OK" in stream.readline(8192), "/info did not return 200 OK")
                headers = {}
                for _ in range(32):
                    line = stream.readline(8192)
                    if line == b"\r\n": break
                    key, value = line.split(b":", 1)
                    headers[key.lower()] = value.strip()
                length = int(headers[b"content-length"])
                require(0 < length <= 65536, "/info body length is invalid")
                info = plistlib.loads(stream.read(length))
                display = info["displays"][0]
                require(display["rotation"] is args.expect_rotation and
                        bool(info["features"] & (1 << 8)) == args.expect_rotation,
                        "Rotation display flag and feature bit disagree")
                require((display["width"], display["height"], display["maxFPS"]) == (width, height, fps), "Advertised display profile is incorrect")
                require((display["widthPixels"], display["heightPixels"]) == (width, height),
                        "Advertised pixel dimensions disagree with the display hint")
                require(bool(info["audioLatencies"]) and all(v["outputLatencyMicros"] == args.latency_ms * 1000 for v in info["audioLatencies"]), "Advertised audio playout latency is incorrect")
                require(bool(info["features"] & (1 << 42)) == high, "HEVC capability flag is incorrect")
            require(process.wait(timeout=10) == 0, "Receiver exit failed")
            checked.append({"width": width, "height": height, "fps": fps, "hevc": high})
            print(f"PASS: /info {width}x{height} at {fps} fps; HEVC={high}; advertised_latency={args.latency_ms}ms; clean silent exit")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
result.write_text(json.dumps({"executable": str(exe), "sha256": executable_hash,
                             "playout_ms": args.latency_ms,
                             "fps": args.fps,
                             "rotation_advertisement": args.expect_rotation,
                             "display_aspect": args.display_aspect,
                             "profiles": checked,
                             "evidence": "Silent loopback /info and timed exit. Initial display hints do not constrain rotated stream dimensions; no picture, rotation or phone acceptance is established."}, indent=2))
aspect_fixture.unlink(missing_ok=True)
