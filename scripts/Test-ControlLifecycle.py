"""Bounded silent RTSP lifecycle checks against one test-created loopback receiver.

No media transport is established, no PCM is submitted, and no UI is automated.
Pair-setup here tests owner admission; it is not authenticated iPhone negotiation.
"""
from __future__ import annotations

import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import plistlib
import re
import socket
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
MAX_HEADER, MAX_BODY = 8192, 65536
RUN_SECONDS = 8


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def ipv4_listeners(port):
    """Read a bounded native TCP table; never infer closure from a connect timeout."""
    # GetExtendedTcpTable(AF_INET, TCP_TABLE_OWNER_PID_LISTENER) returns a
    # DWORD count followed by DWORD-only MIB_TCPROW_OWNER_PID records.
    # https://learn.microsoft.com/windows/win32/api/iphlpapi/nf-iphlpapi-getextendedtcptable
    class Row(ctypes.Structure):
        _fields_ = [(name, ctypes.c_uint32) for name in
                    ("state", "local_address", "local_port", "remote_address", "remote_port", "pid")]

    class Table(ctypes.Structure):
        _fields_ = [("count", ctypes.c_uint32), ("rows", Row * 1)]

    query = ctypes.WinDLL("iphlpapi.dll").GetExtendedTcpTable
    query.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_int,
                      ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
    query.restype = ctypes.c_uint32
    size = ctypes.c_uint32(0)
    status = query(None, ctypes.byref(size), 0, socket.AF_INET, 3, 0)
    require(status == 122, f"TCP table size query failed: {status}")
    for _ in range(3):
        require(Table.rows.offset <= size.value <= 1024 * 1024, "TCP table exceeds one-MiB budget")
        buffer = ctypes.create_string_buffer(size.value)
        status = query(buffer, ctypes.byref(size), 0, socket.AF_INET, 3, 0)
        if status == 122:
            continue
        require(status == 0, f"TCP table query failed: {status}")
        count = ctypes.c_uint32.from_buffer(buffer).value
        require(Table.rows.offset + count * ctypes.sizeof(Row) <= min(size.value, len(buffer)),
                "TCP table row count exceeds returned buffer")
        found = []
        for index in range(count):
            row = Row.from_buffer_copy(buffer, Table.rows.offset + index * ctypes.sizeof(Row))
            if row.state == 2 and socket.ntohs(row.local_port & 0xffff) == port:
                found.append({"pid": row.pid,
                              "address": socket.inet_ntoa(struct.pack("=I", row.local_address)),
                              "port": port})
        return found
    raise RuntimeError("TCP listener table kept growing during bounded retries")


def request_bytes(method, target, cseq, body=b"", version="RTSP/1.0", content_type="application/octet-stream"):
    return (f"{method} {target} {version}\r\nCSeq: {cseq}\r\n"
            f"Content-Type: {content_type}\r\nContent-Length: {len(body)}\r\n\r\n").encode("ascii") + body


class Wire:
    next_connection = 0.0

    def __init__(self, port):
        # Exercise lifecycle rather than tripping the independent 16 accepts/s
        # admission limiter. Tests remain bounded to at most three owner cycles.
        time.sleep(max(0, Wire.next_connection - time.monotonic()))
        Wire.next_connection = time.monotonic() + .08
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=2)
        self.socket.settimeout(2)
        self.pending = bytearray()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.socket.close()

    def read_more(self):
        block = self.socket.recv(4096)
        require(block, "receiver closed before a complete response")
        self.pending.extend(block)
        require(len(self.pending) <= MAX_HEADER + MAX_BODY, "response budget exceeded")

    def response(self, cseq, version="RTSP/1.0"):
        while b"\r\n\r\n" not in self.pending:
            require(len(self.pending) < MAX_HEADER, "response header exceeds budget")
            self.read_more()
        end = self.pending.index(b"\r\n\r\n") + 4
        require(end <= MAX_HEADER, "response header exceeds budget")
        lines = bytes(self.pending[:end - 4]).decode("ascii").split("\r\n")
        parts = lines[0].split(" ", 2)
        require(len(parts) == 3 and parts[0] == version and parts[1].isdigit(), "invalid response status")
        require(len(lines) <= 64, "response header count exceeds budget")
        headers = {}
        for line in lines[1:]:
            key, separator, value = line.partition(":")
            key = key.lower()
            require(separator and key not in headers, "invalid or duplicate response header")
            headers[key] = value.strip()
        require(headers.get("cseq") == str(cseq), "CSeq response did not match its request")
        length = headers.get("content-length", "")
        require(length.isdigit() and int(length) <= MAX_BODY, "invalid response body size")
        length = int(length)
        while len(self.pending) < end + length:
            self.read_more()
        body = bytes(self.pending[end:end + length])
        del self.pending[:end + length]
        return int(parts[1]), headers, body

    def request(self, method, target, cseq, body=b"", **kwargs):
        self.socket.sendall(request_bytes(method, target, cseq, body, **kwargs))
        return self.response(cseq, kwargs.get("version", "RTSP/1.0"))

    def closed(self):
        require(not self.pending, "unconsumed response before expected close")
        try:
            require(self.socket.recv(1) == b"", "receiver did not close rejected/torn-down connection")
        except ConnectionResetError:
            pass


def expect(response, status):
    require(response[0] == status, f"expected status {status}, received {response[0]}")
    return response


def check_info(body):
    info = plistlib.loads(body)
    display = info["displays"][0]
    require((display["width"], display["height"], display["widthPixels"], display["heightPixels"], display["maxFPS"])
            == (1920, 1080, 1920, 1080, 60), "compatibility profile changed across control requests")
    require(info["audioLatencies"] and all(row["outputLatencyMicros"] == 80000 for row in info["audioLatencies"]),
            "shared advertised latency changed")


def run_checks(port, cycles):
    checks = []
    with Wire(port) as client:
        client.socket.sendall(request_bytes("OPTIONS", "*", 101) + request_bytes("GET", "/info?compat=1", 102))
        _, headers, _ = expect(client.response(101), 200)
        require("TEARDOWN" in headers.get("public", ""), "OPTIONS omitted supported controls")
        check_info(expect(client.response(102), 200)[2])
        check_info(expect(client.request("POST", "/info", 103, version="HTTP/1.1"), 200)[2])
        expect(client.request("GET", "/unsupported-control", 104), 404)
        expect(client.request("OPTIONS", "*", 105), 200)
    checks.append("persistent pipelined responses, CSeq/body framing, GET/POST info, HTTP version echo, unsupported endpoint survival")

    for method, target, body in [
        ("RECORD", "rtsp://localhost/1", b""),
        ("GET_PARAMETER", "rtsp://localhost/1", b"volume\r\n"),
        ("SET_PARAMETER", "rtsp://localhost/1", b"volume: -20\r\n"),
        ("FLUSH", "rtsp://localhost/1", b""),
        ("POST", "/audioMode", plistlib.dumps({"audioMode": "default"}, fmt=plistlib.FMT_BINARY)),
        ("SETUP", "rtsp://localhost/1", plistlib.dumps({"streams": [{"type": 110}]}, fmt=plistlib.FMT_BINARY)),
    ]:
        with Wire(port) as client:
            expect(client.request(method, target, 201, body), 455)
            client.closed()
    checks.append("pre-SETUP/proof RECORD, parameters, FLUSH, audio-mode and stream SETUP reject and close")

    audio_teardown = plistlib.dumps({"streams": [{"type": 96}]}, fmt=plistlib.FMT_BINARY)
    for cycle in range(cycles):
        with Wire(port) as owner:
            request = request_bytes("POST", "/pair-setup", 301, bytes(32))
            # Separate client writes exercise stream reassembly; TCP may still
            # coalesce segments, so this does not claim exhaustive fragmentation.
            for part in (request[:13], request[13:-29], request[-29:]):
                owner.socket.sendall(part)
                time.sleep(.01)
            require(len(expect(owner.response(301), 200)[2]) == 32, "pair-setup public-key size")
            with Wire(port) as competitor:
                expect(competitor.request("POST", "/pair-setup", 302, bytes(32)), 453)
                competitor.closed()
            expect(owner.request("TEARDOWN", "rtsp://localhost/1", 303, audio_teardown), 200)
            expect(owner.request("OPTIONS", "*", 304), 200)
            check_info(expect(owner.request("GET", "/info", 307), 200)[2])
            with Wire(port) as competitor:
                expect(competitor.request("POST", "/pair-setup", 305, bytes(32)), 453)
                competitor.closed()
            expect(owner.request("TEARDOWN", "rtsp://localhost/1", 306), 200)
            owner.closed()
        with Wire(port) as observer:
            check_info(expect(observer.request("GET", "/info", 308), 200)[2])
        # Next cycle immediately reclaims after the acknowledged full teardown.
        checks.append(f"owner cycle {cycle + 1}: split writes, second-owner denial, audio-only teardown retains owner, full teardown closes, baseline display info preserved")

    for header in [b"Content-Length: 65537", b"Content-Length: -1",
                   b"Content-Length: 2\r\nContent-Length: 2", b"Transfer-Encoding: chunked"]:
        with Wire(port) as client:
            client.socket.sendall(b"POST /pair-setup RTSP/1.0\r\nCSeq: 401\r\n" + header + b"\r\n\r\n")
            client.closed()
    with Wire(port) as owner:
        expect(owner.request("POST", "/pair-setup", 402, bytes(32)), 200)
        expect(owner.request("TEARDOWN", "rtsp://localhost/1", 403), 200)
        owner.closed()
    checks.append("malformed framing closes; a fresh owner is admitted afterwards")
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--cycles", type=int, choices=(1, 2, 3), default=3)
    args = parser.parse_args()
    executable = args.exe.resolve(strict=True)
    data = ROOT / "local/control-lifecycle" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    data.mkdir(parents=True, exist_ok=False)
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    with (data / "stdout.txt").open("x") as stdout, (data / "stderr.txt").open("x") as stderr:
        subprocess.run([str(executable), "--provision-worker"], check=True, timeout=10,
                       stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, creationflags=flags)
        process = subprocess.Popen([str(executable), "--hidden", "--no-discovery", "--bind", "127.0.0.1",
                                    "--port", "0", "--seconds", str(RUN_SECONDS), "--resolution", "1080",
                                    "--fps", "60", "--latency-ms", "80", "--data-dir", str(data)],
                                   stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, creationflags=flags)
        try:
            deadline = time.monotonic() + 10
            while True:
                require(process.poll() is None, "receiver exited before readiness; inspect test stderr")
                require(time.monotonic() < deadline, "listener readiness deadline")
                log_path = data / "receiver.log"
                text = log_path.read_text(encoding="utf-8") if log_path.exists() else ""
                require(len(text) <= 1024 * 1024, "receiver log budget")
                match = re.search(r"listening address=127\.0\.0\.1 port=(\d+) discovery=false;", text)
                if match:
                    port = int(match[1])
                    with Wire(port) as client:
                        expect(client.request("OPTIONS", "*", 1), 200)
                    break
                time.sleep(.025)
            ready = time.monotonic()
            listeners_before = ipv4_listeners(port)
            require(any(row["pid"] == process.pid and row["address"] == "127.0.0.1" for row in listeners_before),
                    "native TCP table did not identify the test-owned listener")
            checks = run_checks(port, args.cycles)
            require(time.monotonic() - ready < RUN_SECONDS - 2, "control cases exceeded their bounded setup window")
            time.sleep(max(0, RUN_SECONDS - 2 - (time.monotonic() - ready)))
            with Wire(port) as idle, Wire(port) as partial:
                expect(idle.request("OPTIONS", "*", 501), 200)
                partial.socket.sendall(b"POST /pair-setup RTSP/1.0\r\nCSeq: 502\r\nContent-Length: 32\r\n\r\nx")
                began = time.monotonic()
                require(process.wait(timeout=3.5) == 0, "receiver failed its timed shutdown")
                shutdown_wait = time.monotonic() - began
                idle.closed()
                partial.closed()
            checks.append("timed shutdown cancels both idle and partial-body reads before the five-second request deadline")
            require(not ipv4_listeners(port), "native TCP table still contains the test receiver port after shutdown")
            text = (data / "receiver.log").read_text(encoding="utf-8")
            for counter in ("audio_samples", "audio_silence_frames"):
                values = re.findall(rf"\b{counter}=(\d+)", text)
                require(values and all(int(value) == 0 for value in values), f"unexpected audio endpoint data: {counter}")
            require("receiver stopped; all session threads joined" in text, "session threads did not join cleanly")
            result = {"executable": str(executable), "sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                      "cycles": args.cycles, "port": port, "checks": checks,
                      "shutdown_wait_after_partial_request_ms": shutdown_wait * 1000,
                      "listener_before": listeners_before,
                      "listener_after": [],
                      "listener_verification": "Native IPv4 owner-PID LISTEN table identifies the child before testing and is empty for its port after exit 0 plus both existing sockets closing. A connection timeout is not used as proof.",
                      "audio_samples": 0, "audio_silence_frames": 0,
                      "evidence": "Real loopback control listener, no media transport or UI automation. Does not establish authenticated phone negotiation, picture rotation, playback, audible sync, or every physical interaction."}
            (data / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            print(f"PASS: {data / 'result.json'}")
        finally:
            if process.poll() is None:
                process.kill()  # Only the child created by this invocation.
                process.wait(timeout=5)


if __name__ == "__main__":
    main()
