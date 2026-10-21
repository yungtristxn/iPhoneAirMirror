"""Silent real /info wire checks; only processes/listeners created by this test."""
import pathlib
import plistlib
import socket
import subprocess
import time

root = pathlib.Path(__file__).resolve().parent.parent
exe = root / "target/release/airmirror-lite.exe"
data = root / "local/profile-test"
data.mkdir(parents=True, exist_ok=True)
for height, width, fps, high in [(1080, 1920, 60, False), (1440, 2560, 60, True), (2160, 3840, 60, True)]:
    with (data / f"{height}-out.txt").open("w") as out, (data / f"{height}-err.txt").open("w") as err:
        process = subprocess.Popen([str(exe), "--hidden", "--no-discovery", "--bind", "127.0.0.1",
                                    "--port", "17003", "--seconds", "3", "--resolution", str(height),
                                    "--data-dir", str(data)], stdin=subprocess.DEVNULL,
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
                assert b"200 OK" in stream.readline(8192)
                headers = {}
                for _ in range(32):
                    line = stream.readline(8192)
                    if line == b"\r\n": break
                    key, value = line.split(b":", 1)
                    headers[key.lower()] = value.strip()
                length = int(headers[b"content-length"])
                assert 0 < length <= 65536
                info = plistlib.loads(stream.read(length))
                display = info["displays"][0]
                assert (display["width"], display["height"], display["maxFPS"]) == (width, height, fps)
                assert all(v["outputLatencyMicros"]==80000 for v in info["audioLatencies"])
                assert bool(info["features"] & (1 << 42)) == high
            assert process.wait(timeout=10) == 0
            print(f"PASS: /info {width}x{height} at {fps} fps; HEVC={high}; clean silent exit")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
