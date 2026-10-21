"""Measure fresh-process listener readiness, without media or desktop automation."""
import argparse
import json
import pathlib
import socket
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--exe", required=True)
parser.add_argument("--label", required=True)
parser.add_argument("--discovery", action="store_true")
parser.add_argument("--address", default="127.0.0.1")
args = parser.parse_args()
if args.discovery and args.address == "127.0.0.1":
    parser.error("--discovery requires --address with the PC's private LAN IPv4 address")
if args.discovery:
    try:
        with socket.create_connection((args.address, 7000), timeout=.2):
            raise RuntimeError("Close the normal receiver before a discovery benchmark")
    except OSError:
        pass
root = pathlib.Path(__file__).resolve().parent.parent
folder = root / "local" / ("startup-" + args.label)
folder.mkdir(parents=True, exist_ok=True)
samples = []
for trial in range(3):
    with (folder / f"{trial}-out.txt").open("w") as out, (folder / f"{trial}-err.txt").open("w") as err:
        command = [args.exe, "--hidden", "--seconds", "2", "--port", "17005", "--data-dir", str(folder)]
        if not args.discovery:
            command += ["--no-discovery", "--bind", "127.0.0.1"]
        address = args.address
        started = time.perf_counter()
        process = subprocess.Popen(command, stdout=out, stderr=err, stdin=subprocess.DEVNULL,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            while True:
                if process.poll() is not None or time.perf_counter() - started > 15:
                    raise RuntimeError("Receiver did not become ready")
                try:
                    with socket.create_connection((address, 17005), timeout=.1) as connection:
                        connection.settimeout(.1)
                        connection.sendall(b"OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n")
                        if b"200 OK" in connection.recv(2048):
                            break
                except OSError:
                    time.sleep(.01)
            elapsed = (time.perf_counter() - started) * 1000
            assert process.wait(timeout=10) == 0
            samples.append(round(elapsed, 1))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
summary = {"listener_ready_ms": samples, "discovery": args.discovery,
           "evidence": "Fresh processes, existing OS caches; no phone, media or sound"}
(folder / "result.json").write_text(json.dumps(summary, indent=2))
print(json.dumps(summary))
