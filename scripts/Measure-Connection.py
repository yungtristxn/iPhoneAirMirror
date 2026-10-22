"""Silent authenticated loopback negotiation benchmark using public test vectors.

Requires Python's cryptography package. No audio SETUP, media payload, discovery,
phone access or desktop automation. Measures receiver work, not tap-to-picture.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import re
import socket
import statistics
import subprocess
import threading
import time

from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("control_lifecycle", ROOT / "scripts/Test-ControlLifecycle.py")
wire_helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wire_helpers)
Wire, expect, require = wire_helpers.Wire, wire_helpers.expect, wire_helpers.require


def timing(sock, stop, failures):
    try:
        while not stop.is_set():
            try:
                data, address = sock.recvfrom(64)
            except socket.timeout:
                continue
            if len(data) == 32 and data[1] & 0x7f == 0x52:
                stamp = int(time.perf_counter() * (1 << 32)).to_bytes(8, "big")
                sock.sendto(bytes([0x80, 0xd3, 0, 7]) + bytes(4) + data[24:32] + stamp + stamp, address)
    except Exception as error:
        failures.append(type(error).__name__)


def session(port, data_dir):
    key = ed25519.Ed25519PrivateKey.generate()
    curve = x25519.X25519PrivateKey.generate()
    public = key.public_key().public_bytes_raw()
    curve_public = curve.public_key().public_bytes_raw()
    fixture = (ROOT / "tests/fixtures/fairplay-known-answer.bin").read_bytes()
    require(len(fixture) == 252, "Public FairPlay fixture length")
    message, encrypted = fixture[:164], fixture[164:236]
    stop, errors = threading.Event(), []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as ntp, Wire(port) as wire:
        wire.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        ntp.bind(("127.0.0.1", 0))
        ntp.settimeout(.05)
        worker = threading.Thread(target=timing, args=(ntp, stop, errors))
        worker.start()
        event = video = None
        try:
            started = time.perf_counter()
            expect(wire.request("OPTIONS", "*", 1), 200)
            options_ms = (time.perf_counter() - started) * 1000
            claimed = time.perf_counter()
            receiver_key = expect(wire.request("POST", "/pair-setup", 2, public), 200)[2]
            response = expect(wire.request("POST", "/pair-verify", 3,
                                          bytes([1, 0, 0, 0]) + curve_public + public), 200)[2]
            require(len(response) == 96, "Pair-verify response size")
            shared = curve.exchange(x25519.X25519PublicKey.from_public_bytes(response[:32]))
            aes_key = hashlib.sha512(b"Pair-Verify-AES-Key" + shared).digest()[:16]
            aes_iv = hashlib.sha512(b"Pair-Verify-AES-IV" + shared).digest()[:16]
            cipher = Cipher(algorithms.AES(aes_key), modes.CTR(aes_iv)).encryptor()
            signature = cipher.update(response[32:])
            ed25519.Ed25519PublicKey.from_public_bytes(receiver_key).verify(signature, response[:32] + curve_public)
            proof = cipher.update(key.sign(curve_public + response[:32]))
            expect(wire.request("POST", "/pair-verify", 4, bytes(4) + proof), 200)
            first = bytearray(b"FPLY\x03\x01\x01\x00\x00\x00\x00\x04\x00\x00\x00\x00")
            first[14] = message[12]
            expect(wire.request("POST", "/fp-setup", 5, bytes(first)), 200)
            expect(wire.request("POST", "/fp-setup", 6, message), 200)
            setup = plistlib.dumps({"ekey": encrypted, "eiv": bytes(16),
                                   "timingPort": ntp.getsockname()[1], "timingProtocol": "NTP"},
                                  fmt=plistlib.FMT_BINARY)
            started = time.perf_counter()
            ports = plistlib.loads(expect(wire.request("SETUP", "rtsp://localhost/1", 7, setup), 200)[2])
            setup_ms = (time.perf_counter() - started) * 1000
            if ports.get("eventPort"):
                event = socket.create_connection(("127.0.0.1", ports["eventPort"]), timeout=2)
            expect(wire.request("RECORD", "rtsp://localhost/1", 8), 200)
            request = plistlib.dumps({"streams": [{"type": 110, "streamConnectionID": 42}]}, fmt=plistlib.FMT_BINARY)
            ports = plistlib.loads(expect(wire.request("SETUP", "rtsp://localhost/1", 9, request), 200)[2])
            negotiation_ms = (time.perf_counter() - claimed) * 1000
            video = socket.create_connection(("127.0.0.1", ports["streams"][0]["dataPort"]), timeout=2)
            # Excludes log polling, but includes response delivery, client connect
            # and accepted-stream configuration. Log stamps have 1 ms resolution.
            deadline = time.monotonic() + 2
            while True:
                lines = (data_dir / "receiver.log").read_text(encoding="utf-8").splitlines()
                ready = [float(s.split()[0]) for s in lines if "video transport ready" in s]
                connected = [float(s.split()[0]) for s in lines if "video TCP connected" in s]
                if ready and len(connected) == len(ready):
                    video_accept_ms = (connected[-1] - ready[-1]) * 1000
                    break
                require(time.monotonic() < deadline, "Video TCP acceptance deadline")
                time.sleep(.002)
            expect(wire.request("TEARDOWN", "rtsp://localhost/1", 10), 200)
            wire.closed()
            return {"control_options_ms": round(options_ms, 3), "encrypted_setup_ms": round(setup_ms, 3),
                    "claim_to_video_setup_ms": round(negotiation_ms, 3),
                    "video_transport_to_accept_ms": round(video_accept_ms, 3)}
        finally:
            if video:
                video.close()
            if event:
                event.close()
            stop.set()
            worker.join(timeout=1)
            require(not worker.is_alive() and not errors, "Synthetic timing responder failed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--sessions", type=int, default=3)
    args = parser.parse_args()
    require(re.fullmatch(r"[A-Za-z0-9._-]{1,64}", args.label), "Invalid result label")
    require(1 <= args.trials <= 10 and 1 <= args.sessions <= 5, "Bounded trial/session counts required")
    output = ROOT / "local" / ("connection-" + args.label)
    output.mkdir(exist_ok=False)
    exe = args.exe.resolve(strict=True)
    subprocess.run([str(exe), "--provision-worker"], check=True, timeout=10,
                   stdin=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
    samples = []
    for trial in range(args.trials):
        data = output / str(trial)
        data.mkdir()
        with (data / "stdout.txt").open("w") as out, (data / "stderr.txt").open("w") as err:
            process = subprocess.Popen([str(exe), "--hidden", "--no-discovery", "--bind", "127.0.0.1",
                                        "--port", "0", "--seconds", "6", "--resolution", "1080",
                                        "--data-dir", str(data)], stdout=out, stderr=err,
                                       stdin=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                deadline = time.monotonic() + 10
                while True:
                    require(process.poll() is None and time.monotonic() < deadline, "Receiver startup deadline")
                    log = (data / "receiver.log").read_text(encoding="utf-8") if (data / "receiver.log").exists() else ""
                    found = re.search(r"listening address=127\.0\.0\.1 port=(\d+) discovery=false", log)
                    if found:
                        port = int(found[1])
                        break
                    time.sleep(.005)
                for ordinal in range(args.sessions):
                    samples.append({"trial": trial, "session": ordinal, **session(port, data)})
                    time.sleep(.15)  # Respect connection/request-rate bounds.
                require(process.wait(timeout=10) == 0, "Receiver exit failed")
                log = (data / "receiver.log").read_text(encoding="utf-8")
                require("receiver stopped; all session threads joined" in log, "Shutdown incomplete")
                require("audio_samples=0" in log and not re.search(r"audio_samples=[1-9]", log), "Audio was submitted")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
    medians = {group: {field: round(statistics.median(s[field] for s in samples if predicate(s)), 3)
                       for field in samples[0] if field.endswith("_ms")}
               for group, predicate in [("first_session", lambda s: s["session"] == 0),
                                        ("all_sessions", lambda s: True)]}
    result = {"timeUtc": datetime.now(timezone.utc).isoformat(), "exe": str(exe),
              "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "samples": samples, "medians": medians,
              "video_transport_clock_resolution_ms": 1,
              "evidence": "Authenticated synthetic loopback exchange including Python sender work; video transport metric includes SETUP reply/client connect and uses 1 ms log stamps (zero means below resolution). No media payload, iPhone, sound or end-to-end picture latency"}
    (output / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"result": str(output / "result.json"), "medians": medians}, indent=2))


if __name__ == "__main__":
    main()
