"""Independent, silent HAP transient-pairing/control regression.

SRP uses Python integer arithmetic only as a test oracle. Production must use its
maintained crypto implementation. Audio SETUP exercises isolated decoder startup;
no RTP audio, encoded media payload, PCM, phone, discovery or UI automation is
performed. Requires the existing cryptography package.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
from datetime import datetime, timezone
import hashlib
import hmac
import importlib.util
import json
from pathlib import Path
import plistlib
import re
import secrets
import socket
import struct
import subprocess
import threading
import time

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests/fixtures/hap-srp-vector.json"
MAX_HEADER, MAX_BODY, MAX_RECORD = 8192, 65536, 1024
IO_SECONDS, RUN_SECONDS, CODEC_READY_SECONDS = 3.0, 35, 8.0
GROUP = json.loads(FIXTURE.read_text(encoding="utf-8"))["group"]
N, G, WIDTH = int(GROUP["prime_hex"], 16), GROUP["generator"], 384


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class AuthenticationError(RuntimeError):
    pass


class ClosedBeforeResponse(EOFError):
    pass


def sha(data):
    return hashlib.sha512(data).digest()


def integer(data):
    return int.from_bytes(data, "big")


def pad(value):
    return value.to_bytes(WIDTH, "big")


def minimal(value):
    return value.to_bytes((value.bit_length() + 7) // 8, "big")


def proofs(username, salt, public_a, public_b, key):
    prefix = bytes(a ^ b for a, b in zip(sha(pad(N)), sha(minimal(G))))
    client = sha(prefix + sha(username) + salt + minimal(public_a) + minimal(public_b) + key)
    # Apple HAP's M2 uses padded A even though its M1 uses minimal A/B.
    return client, sha(pad(public_a) + client + key)


def srp_client(salt, server_public, private_a, username=b"Pair-Setup", password=b"3939"):
    require(len(salt) == 16 and len(server_public) == WIDTH, "Invalid SRP challenge width")
    public_b = integer(server_public)
    require(0 < public_b < N, "Invalid SRP server public value")
    public_a = pow(G, private_a, N)
    x = integer(sha(salt + sha(username + b":" + password)))
    multiplier = integer(sha(pad(N) + pad(G)))
    u = integer(sha(pad(public_a) + pad(public_b)))
    require(u != 0, "Invalid SRP scrambling parameter")
    secret = pow((public_b - multiplier * pow(G, x, N)) % N, private_a + u*x, N)
    key = sha(minimal(secret))
    proof, server_proof = proofs(username, salt, public_a, public_b, key)
    return public_a, proof, server_proof, key, secret


def check_fixtures():
    fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
    require(N.bit_length() == 3072 and G == 5, "Unexpected SRP group")
    for row in [fixture["vector"], *fixture.get("derived_vectors", [])]:
        salt, user, password = bytes.fromhex(row["salt_hex"]), row["username"].encode(), row["password"].encode()
        public, proof, server_proof, key, secret = srp_client(
            salt, bytes.fromhex(row["server_public_hex"]), int(row["client_private_hex"], 16), user, password)
        expected = {"client_public_hex": pad(public), "client_proof_hex": proof,
                    "server_proof_hex": server_proof, "session_key_hex": key, "premaster_secret_hex": pad(secret)}
        x = integer(sha(salt + sha(user + b":" + password)))
        verifier = pow(G, x, N)
        multiplier = integer(sha(pad(N) + pad(G)))
        server = (multiplier*verifier + pow(G, int(row["server_private_hex"], 16), N)) % N
        expected.update(verifier_hex=pad(verifier), server_public_hex=pad(server),
                        scrambling_u_hex=sha(pad(public) + pad(server)))
        for field, value in expected.items():
            require(value.hex() == row[field], f"Public fixture mismatch: {row['id']} {field}")
    for row in fixture["padding_regressions"]:
        x = integer(sha(bytes.fromhex(row["salt_hex"]) + sha(row["username"].encode() + b":" + row["password"].encode())))
        require(pad(pow(G, x, N)).hex() == row["verifier_hex"], "Padded verifier fixture mismatch")
    return 1 + len(fixture.get("derived_vectors", []))


def tlv_encode(fields):
    result = bytearray()
    for kind, value in fields.items():
        require(0 <= kind <= 255 and 0 < len(value) <= 4096, "Test TLV input budget")
        for offset in range(0, len(value), 255):
            chunk = value[offset:offset + 255]
            result.extend(bytes((kind, len(chunk))) + chunk)
    return bytes(result)


def tlv_decode(data):
    require(len(data) <= 4096, "Pairing TLV response budget")
    result, offset, previous, previous_length = {}, 0, None, None
    while offset < len(data):
        require(offset + 2 <= len(data), "Truncated TLV header")
        kind, length = data[offset:offset + 2]
        offset += 2
        require(length and offset + length <= len(data), "Truncated or empty TLV value")
        if kind in result:
            require(kind == previous and previous_length == 255, "Ambiguous duplicate TLV field")
        result[kind] = result.get(kind, b"") + data[offset:offset + length]
        offset += length
        previous, previous_length = kind, length
    return result


def verify_m4(fields, expected):
    if fields.get(6) != b"\x04" or 7 in fields or len(fields.get(4, b"")) != 64:
        raise AuthenticationError("Invalid M4 state/error/proof width")
    if not hmac.compare_digest(fields[4], expected):
        raise AuthenticationError("M4 server proof mismatch")


def derive(key, info):
    return HKDF(algorithm=hashes.SHA512(), length=32, salt=b"Control-Salt", info=info).derive(key)


class Records:
    def __init__(self, key):
        self.write_key = derive(key, b"Control-Write-Encryption-Key")
        self.read_key = derive(key, b"Control-Read-Encryption-Key")
        self.out_counter = self.in_counter = 0
        self.pending = bytearray()
        self.received_records = self.sent_records = 0

    @staticmethod
    def nonce(counter):
        require(0 <= counter < 1 << 64, "Record nonce exhausted")
        return bytes(4) + counter.to_bytes(8, "little")

    def encode(self, data):
        result = bytearray()
        for offset in range(0, len(data), MAX_RECORD):
            chunk = data[offset:offset + MAX_RECORD]
            prefix = len(chunk).to_bytes(2, "little")
            result.extend(prefix + ChaCha20Poly1305(self.write_key).encrypt(self.nonce(self.out_counter), chunk, prefix))
            self.out_counter += 1
            self.sent_records += 1
        return bytes(result)

    def feed(self, data):
        self.pending.extend(data)
        require(len(self.pending) <= MAX_HEADER + MAX_BODY, "Encrypted receive buffer budget")
        result = bytearray()
        while len(self.pending) >= 2:
            prefix = bytes(self.pending[:2])
            length = int.from_bytes(prefix, "little")
            require(0 < length <= MAX_RECORD, "Encrypted response record length")
            if len(self.pending) < length + 18:
                break
            ciphertext = bytes(self.pending[2:length + 18])
            plaintext = ChaCha20Poly1305(self.read_key).decrypt(self.nonce(self.in_counter), ciphertext, prefix)
            self.in_counter += 1
            self.received_records += 1
            del self.pending[:length + 18]
            result.extend(plaintext)
        return bytes(result)


class Wire:
    next_connection = 0.0

    def __init__(self, port):
        time.sleep(max(0.0, self.next_connection - time.monotonic()))
        Wire.next_connection = time.monotonic() + .08
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=IO_SECONDS)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.socket.settimeout(IO_SECONDS)
        self.pending, self.cseq, self.records = bytearray(), 0, None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.socket.close()

    def request_bytes(self, method, target, body=b"", headers=None):
        self.cseq += 1
        fields = {"CSeq": str(self.cseq), "Content-Length": str(len(body)),
                  "Content-Type": "application/octet-stream", **(headers or {})}
        return (f"{method} {target} RTSP/1.0\r\n" + "".join(f"{key}: {value}\r\n" for key, value in fields.items()) + "\r\n").encode() + body

    def more(self, deadline):
        require(time.monotonic() < deadline, "Response deadline")
        self.socket.settimeout(max(.001, deadline - time.monotonic()))
        block = self.socket.recv(4096)
        if not block and not self.pending and (self.records is None or not self.records.pending):
            raise ClosedBeforeResponse("Connection closed without a response")
        require(block, "Connection closed before response completed")
        self.pending.extend(self.records.feed(block) if self.records else block)
        require(len(self.pending) <= MAX_HEADER + MAX_BODY, "Plain response buffer budget")

    def response(self):
        deadline = time.monotonic() + IO_SECONDS
        while b"\r\n\r\n" not in self.pending:
            require(len(self.pending) < MAX_HEADER, "Response header budget")
            self.more(deadline)
        end = self.pending.index(b"\r\n\r\n") + 4
        require(end <= MAX_HEADER, "Response header budget")
        lines = bytes(self.pending[:end-4]).decode("ascii").split("\r\n")
        parts = lines[0].split(" ", 2)
        require(len(parts) == 3 and parts[0] == "RTSP/1.0" and parts[1].isdigit(), "Invalid response status")
        headers = {}
        require(len(lines) <= 64, "Response header count")
        for line in lines[1:]:
            name, sep, value = line.partition(":")
            require(sep and name.lower() not in headers, "Duplicate or malformed response header")
            headers[name.lower()] = value.strip()
        require(headers.get("cseq") == str(self.cseq), "Response CSeq mismatch")
        size = headers.get("content-length", "")
        require(size.isdigit() and int(size) <= MAX_BODY, "Response body length")
        size = int(size)
        while len(self.pending) < end + size:
            self.more(deadline)
        body = bytes(self.pending[end:end + size])
        del self.pending[:end + size]
        return int(parts[1]), body

    def request(self, method, target, body=b"", headers=None, fragmented=False):
        request = self.request_bytes(method, target, body, headers)
        data = self.records.encode(request) if self.records else request
        if fragmented:
            for size in (1, 1, 7, 31, 1021):
                self.socket.sendall(data[:size]); data = data[size:]
                time.sleep(.002)
        if data:
            self.socket.sendall(data)
        return self.response()

    def closed(self):
        require(not self.pending, "Unread response at expected close")
        self.socket.settimeout(IO_SECONDS)
        try:
            require(self.socket.recv(1) == b"", "Connection accepted data after terminal failure")
        except ConnectionResetError:
            pass


def ok(response):
    require(response[0] == 200, f"Expected success, received status {response[0]}")
    return response[1]


def m1(wire, four_byte_flags=False):
    flags = b"\x10\x00\x00\x00" if four_byte_flags else b"\x10"
    data = tlv_encode({0: b"\x00", 6: b"\x01", 19: flags})
    fields = tlv_decode(ok(wire.request("POST", "/pair-setup", data, {"X-Apple-HKP": "4"})))
    require(fields.get(6) == b"\x02" and 7 not in fields, "Invalid M2 state/error")
    require(len(fields.get(2, b"")) == 16 and len(fields.get(3, b"")) == WIDTH, "Invalid M2 challenge widths")
    return fields[2], fields[3]


def pair(wire, *, short_public=False, forged_m4=False, four_byte_flags=False):
    salt, server = m1(wire, four_byte_flags)
    private = 1 if short_public else integer(secrets.token_bytes(32)) or 1
    public, proof, expected, key, _ = srp_client(salt, server, private)
    data = tlv_encode({6: b"\x03", 3: minimal(public) if short_public else pad(public), 4: proof})
    fields = tlv_decode(ok(wire.request("POST", "/pair-setup", data, {"X-Apple-HKP": "4"})))
    if forged_m4:
        verify_m4(fields, expected)
        require(len(fields.get(4, b"")) == 64, "No server proof for forged-M4 control")
        altered = dict(fields); altered[4] = bytes((fields[4][0] ^ 1,)) + fields[4][1:]
        try:
            verify_m4(altered, expected)
        except AuthenticationError:
            require(wire.records is None, "Encryption enabled before server proof validation")
            return data
        raise RuntimeError("Forged M4 was accepted by the independent client")
    verify_m4(fields, expected)
    require(not wire.pending, "Unexpected bytes before plaintext/encrypted boundary")
    wire.records = Records(key)
    return data


def expect_pair_rejection(wire, data):
    try:
        response = wire.request("POST", "/pair-setup", data, {"X-Apple-HKP": "4"})
    except (ClosedBeforeResponse, ConnectionResetError, BrokenPipeError):
        return
    if response[0] == 200:
        fields = tlv_decode(response[1])
        require(7 in fields and fields[7] != b"\x00", "Invalid pairing proof/state was accepted")
    else:
        require(400 <= response[0] <= 599, "Pairing failure had unexpected status")
    wire.closed()


def timing(sock, stop, errors):
    try:
        while not stop.is_set():
            try:
                data, address = sock.recvfrom(64)
            except socket.timeout:
                continue
            if not stop.is_set() and len(data) == 32 and data[1] & 0x7f == 0x52:
                stamp = int(time.perf_counter() * (1 << 32)).to_bytes(8, "big")
                sock.sendto(bytes((0x80, 0xd3, 0, 7)) + bytes(4) + data[24:32] + stamp + stamp, address)
    except Exception as error:
        if not stop.is_set():
            errors.append(type(error).__name__)


@contextmanager
def timing_peer():
    stop, errors = threading.Event(), []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as ntp:
        ntp.bind(("127.0.0.1", 0)); ntp.settimeout(.05)
        worker = threading.Thread(target=timing, args=(ntp, stop, errors), daemon=True)
        worker.start()
        try:
            yield ntp, stop
        finally:
            stop.set(); worker.join(timeout=1)
            require(not worker.is_alive() and not errors, "Synthetic timing responder failed")


def owned_udp_endpoints(process_id, ports):
    """Inspect bound UDP ports without sending audio or probing them with packets."""
    # GetExtendedUdpTable(AF_INET, UDP_TABLE_OWNER_PID): DWORD count followed by
    # MIB_UDPROW_OWNER_PID (local address, network-order local port, owner PID).
    class Row(ctypes.Structure):
        _fields_ = [(name, ctypes.c_uint32) for name in ("address", "port", "pid")]

    class Table(ctypes.Structure):
        _fields_ = [("count", ctypes.c_uint32), ("rows", Row * 1)]

    query = ctypes.WinDLL("iphlpapi.dll").GetExtendedUdpTable
    query.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_int,
                      ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
    query.restype = ctypes.c_uint32
    size = ctypes.c_uint32(0)
    require(query(None, ctypes.byref(size), 0, socket.AF_INET, 1, 0) == 122, "UDP table size query failed")
    for _ in range(3):
        require(Table.rows.offset <= size.value <= 1024 * 1024, "UDP table budget")
        buffer = ctypes.create_string_buffer(size.value)
        status = query(buffer, ctypes.byref(size), 0, socket.AF_INET, 1, 0)
        if status == 122:
            continue
        require(status == 0, f"UDP table query failed: {status}")
        count = ctypes.c_uint32.from_buffer(buffer).value
        require(Table.rows.offset + count * ctypes.sizeof(Row) <= min(size.value, len(buffer)), "UDP table row count")
        found = {}
        for index in range(count):
            row = Row.from_buffer_copy(buffer, Table.rows.offset + index * ctypes.sizeof(Row))
            port = socket.ntohs(row.port & 0xffff)
            if row.pid == process_id and port in ports:
                require(port not in found, "Duplicate owned UDP endpoint")
                found[port] = socket.inet_ntoa(struct.pack("=I", row.address))
        return found
    raise RuntimeError("UDP table kept growing during bounded retries")


def require_bound_udp(process_id, ports):
    found = owned_udp_endpoints(process_id, ports)
    require(set(found) == set(ports) and all(address == "127.0.0.1" for address in found.values()),
            "Negotiated UDP ports are not bound by the test receiver on loopback")


def negotiated_port(value):
    require(type(value) is int and 0 < value < 65536, "Invalid negotiated port")
    return value


def setup_timing(wire, ntp, process_id):
    setup = plistlib.dumps({"timingPort": ntp.getsockname()[1], "timingProtocol": "NTP"}, fmt=plistlib.FMT_BINARY)
    reply = plistlib.loads(ok(wire.request("SETUP", "rtsp://localhost/1", setup)))
    port = negotiated_port(reply.get("timingPort"))
    require_bound_udp(process_id, [port])
    return port


def read_receiver_log(data_dir):
    with (data_dir / "receiver.log").open("rb") as source:
        data = source.read(1024 * 1024 + 1)
    require(len(data) <= 1024 * 1024, "Receiver log budget")
    return data.decode("utf-8")


def codec_ready_count(data_dir, codec):
    name = {8: "AAC-ELD", 4: "AAC-LC"}[codec]
    return len(re.findall(rf"\baudio codec={name} backend=[^\r\n]+ sample_rate=44100 channels=2 AppContainer=yes\b",
                          read_receiver_log(data_dir)))


def wait_codec_ready(data_dir, codec, previous):
    deadline = time.monotonic() + CODEC_READY_SECONDS
    while codec_ready_count(data_dir, codec) <= previous:
        require(time.monotonic() < deadline, "Isolated audio decoder readiness deadline")
        time.sleep(.025)


def audio_control_lifecycle(wire, ntp_port, data_dir, process_id):
    evidence = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as control:
        control.bind(("127.0.0.1", 0))
        # This socket supplies a real bound remote control port only. Nothing is
        # sent to either audio data/control endpoint, including sync or resend.
        for codec, spf, audio_format, key_fields, key_source in [
            (8, 480, 1 << 24, {"shk": bytes(range(32))}, "explicit_shk32"),
            (4, 1024, 1 << 22, {"streamConnectionID": 43}, "derived_stream_id"),
        ]:
            before = codec_ready_count(data_dir, codec)
            stream = {"type": 96, "ct": codec, "spf": spf, "audioFormat": audio_format,
                      "controlPort": control.getsockname()[1], **key_fields}
            body = plistlib.dumps({"streams": [stream]}, fmt=plistlib.FMT_BINARY)
            reply = plistlib.loads(ok(wire.request("SETUP", "rtsp://localhost/1", body)))
            require(len(reply.get("streams", [])) == 1 and reply["streams"][0].get("type") == 96,
                    "Audio SETUP reply stream mismatch")
            ports = [negotiated_port(reply["streams"][0].get(key)) for key in ("dataPort", "controlPort")]
            require(len(set([ntp_port, *ports])) == 3, "Audio/timing UDP ports overlap")
            require_bound_udp(process_id, [ntp_port, *ports])
            wait_codec_ready(data_dir, codec, before)
            ok(wire.request("RECORD", "rtsp://localhost/1"))
            volume = ok(wire.request("GET_PARAMETER", "rtsp://localhost/1", b"volume\r\n"))
            require(volume.startswith(b"volume: "), "Audio control parameter reply missing")
            teardown = plistlib.dumps({"streams": [{"type": 96}]}, fmt=plistlib.FMT_BINARY)
            ok(wire.request("TEARDOWN", "rtsp://localhost/1", teardown))
            require(not owned_udp_endpoints(process_id, ports), "Audio ports survived acknowledged partial teardown")
            require_bound_udp(process_id, [ntp_port])
            ok(wire.request("GET", "/info"))
            evidence.append({"codec": codec, "samples_per_frame": spf, "audio_format": audio_format,
                             "key_source": key_source, "codec_ready_appcontainer": True,
                             "bound_audio_ports": ports, "audio_ports_removed": True,
                             "timing_retained": True, "audio_datagrams_sent": 0})
    return evidence


def lifecycle(wire, data_dir, process_id):
    ok(wire.request("GET", "/info", headers={"X-Test-Padding": "x" * 1300}, fragmented=True))
    require(wire.records.sent_records >= 2, "Multi-record request control not exercised")
    with timing_peer() as (ntp, timing_stop):
        video = None
        try:
            ntp_port = setup_timing(wire, ntp, process_id)
            setup = plistlib.dumps({"streams": [{"type": 110, "streamConnectionID": 42}]}, fmt=plistlib.FMT_BINARY)
            reply = plistlib.loads(ok(wire.request("SETUP", "rtsp://localhost/1", setup)))
            port = reply["streams"][0]["dataPort"]
            require(0 < port < 65536, "Invalid negotiated video port")
            video = socket.create_connection(("127.0.0.1", port), timeout=IO_SECONDS)
            video.settimeout(IO_SECONDS)
            audio = audio_control_lifecycle(wire, ntp_port, data_dir, process_id)
            timing_stop.set()
            ok(wire.request("TEARDOWN", "rtsp://localhost/1")); wire.closed()
            require(not owned_udp_endpoints(process_id, [ntp_port]), "Timing port survived full teardown")
            try:
                require(video.recv(1) == b"", "Video socket survived teardown")
            except ConnectionResetError:
                pass
            return {"sent_records": wire.records.sent_records, "received_records": wire.records.received_records}, audio
        finally:
            if video: video.close()


def reject_audio_keys(port, data_dir, process_id):
    cases = []
    for name, fields in [("short_shk", {"shk": bytes(31)}), ("long_shk", {"shk": bytes(33)}),
                         ("missing_shk_and_stream_id", {})]:
        with Wire(port) as wire, timing_peer() as (ntp, timing_stop), socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as control:
            pair(wire)
            control.bind(("127.0.0.1", 0))
            timing_port = setup_timing(wire, ntp, process_id)
            before = codec_ready_count(data_dir, 8)
            stream = {"type": 96, "ct": 8, "spf": 480, "audioFormat": 1 << 24,
                      "controlPort": control.getsockname()[1], **fields}
            body = plistlib.dumps({"streams": [stream]}, fmt=plistlib.FMT_BINARY)
            timing_stop.set()
            status, _ = wire.request("SETUP", "rtsp://localhost/1", body)
            require(400 <= status <= 599, f"Invalid audio keys accepted: {name}")
            wire.closed()
            require(not owned_udp_endpoints(process_id, [timing_port]), "Rejected audio SETUP retained its timing socket")
            require(codec_ready_count(data_dir, 8) == before, "Invalid audio keys started a decoder")
        cases.append(name)
    return cases


def run_checks(port, data_dir, process_id):
    checks = []
    with Wire(port) as wire:
        info = plistlib.loads(ok(wire.request("GET", "/info")))
        features = info["features"]
        require(isinstance(features, int) and features & (1 << 48) and not features & (1 << 27), "HAP bit48/legacy bit27 advertisement mismatch")
        ok(wire.request("OPTIONS", "*"))
    checks.append("public info/OPTIONS; transient feature48 set and legacy feature27 clear")
    with Wire(port) as wire:
        saved_m3 = pair(wire)
        records, audio = lifecycle(wire, data_dir, process_id)
    checks.append("mutually verified M1-M4; encrypted fragmented/multirecord info; timing/video SETUP; teardown closes control/video")
    checks.append("same encrypted session: AAC-ELD explicit shk32 then AAC-LC derived stream key; isolated codec startup, RECORD/parameters, partial audio teardown and retained timing/control; no audio packets")
    rejected_audio = reject_audio_keys(port, data_dir, process_id)
    checks.append("short/long shk and missing shk/streamConnectionID fail closed without decoder startup; next fresh HAP session remains usable")
    with Wire(port) as wire:
        pair(wire, short_public=True, four_byte_flags=True)
        ok(wire.request("GET", "/info"))
        ok(wire.request("TEARDOWN", "rtsp://localhost/1")); wire.closed()
    checks.append("short padded client public A and four-byte LE transient flags")
    with Wire(port) as wire:
        pair(wire, forged_m4=True)
    checks.append("independent client rejects altered real M4 proof before enabling encryption")
    with Wire(port) as wire:
        salt, server = m1(wire)
        public, proof, _, _, _ = srp_client(salt, server, integer(secrets.token_bytes(32)) or 1)
        expect_pair_rejection(wire, tlv_encode({6: b"\x03", 3: pad(public), 4: bytes((proof[0] ^ 1,)) + proof[1:]}))
    checks.append("wrong client proof rejected terminally")
    for public in (0, N):
        with Wire(port) as wire:
            salt, server = m1(wire)
            proof, _ = proofs(b"Pair-Setup", salt, public, integer(server), sha(b""))
            expect_pair_rejection(wire, tlv_encode({6: b"\x03", 3: pad(public), 4: proof}))
    checks.append("A=0 and A=N rejected even with the degenerate S=0 proof")
    with Wire(port) as wire:
        expect_pair_rejection(wire, saved_m3)
    with Wire(port) as wire:
        m1(wire); expect_pair_rejection(wire, saved_m3)
    checks.append("M3 before M1 and proof replay against a fresh M2 rejected")
    for mutation in ("tag", "ciphertext", "aad", "direction", "replay", "skipped-counter", "oversized-record", "plaintext"):
        with Wire(port) as wire:
            pair(wire)
            request = wire.request_bytes("OPTIONS", "*")
            layer = wire.records
            if mutation == "plaintext":
                data = request
            elif mutation == "oversized-record":
                data = (MAX_RECORD + 1).to_bytes(2, "little")
            elif mutation in ("aad", "direction", "skipped-counter"):
                prefix = len(request).to_bytes(2, "little")
                aad = (len(request) + 1).to_bytes(2, "little") if mutation == "aad" else prefix
                key = layer.read_key if mutation == "direction" else layer.write_key
                counter = 1 if mutation == "skipped-counter" else 0
                data = prefix + ChaCha20Poly1305(key).encrypt(layer.nonce(counter), request, aad)
            else:
                data = layer.encode(request)
                if mutation == "replay":
                    wire.socket.sendall(data); ok(wire.response())
                else:
                    position = -1 if mutation == "tag" else 2
                    changed = bytearray(data); changed[position] ^= 1; data = bytes(changed)
            wire.socket.sendall(data); wire.closed()
        checks.append(f"encrypted-session {mutation} failure closes without plaintext fallback")
    with Wire(port) as wire:
        pair(wire); ok(wire.request("GET", "/info"))
        ok(wire.request("TEARDOWN", "rtsp://localhost/1")); wire.closed()
    checks.append("fresh verified reconnect after negative controls")
    return checks, records, {"modes": audio, "rejected_key_cases": rejected_audio, "audio_datagrams_sent": 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    args = parser.parse_args()
    vector_count = check_fixtures()
    executable = args.exe.resolve(strict=True)
    executable_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
    data = ROOT / "local/hap-pairing" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    data.mkdir(parents=True, exist_ok=False)
    flags = subprocess.CREATE_NO_WINDOW
    with (data / "stdout.txt").open("x") as stdout, (data / "stderr.txt").open("x") as stderr:
        subprocess.run([str(executable), "--provision-worker"], check=True, timeout=10,
                       stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, creationflags=flags)
        child = subprocess.Popen([str(executable), "--hidden", "--mute", "--no-discovery", "--bind", "127.0.0.1",
                                  "--port", "0", "--seconds", str(RUN_SECONDS), "--resolution", "1080", "--data-dir", str(data)],
                                 stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, creationflags=flags)
        try:
            deadline = time.monotonic() + 10
            while True:
                require(child.poll() is None and time.monotonic() < deadline, "Receiver startup deadline; inspect test stderr")
                log_path = data / "receiver.log"
                log = log_path.read_text(encoding="utf-8") if log_path.exists() else ""
                found = re.search(r"listening address=127\.0\.0\.1 port=(\d+) discovery=false", log)
                if found: break
                time.sleep(.025)
            port = int(found[1])
            spec = importlib.util.spec_from_file_location("control_lifecycle", ROOT / "scripts/Test-ControlLifecycle.py")
            helpers = importlib.util.module_from_spec(spec); spec.loader.exec_module(helpers)
            require(any(row["pid"] == child.pid for row in helpers.ipv4_listeners(port)), "Missing owned listener positive control")
            checks, record_counts, audio_control = run_checks(port, data, child.pid)
            require(child.wait(timeout=RUN_SECONDS + 5) == 0, "Receiver timed shutdown failed")
            require(not helpers.ipv4_listeners(port), "Receiver listener survived shutdown")
            log = log_path.read_text(encoding="utf-8")
            for name in ("audio_samples", "audio_silence_frames"):
                counters = re.findall(rf"\b{name}=(\d+)", log)
                require(counters and all(int(value) == 0 for value in counters), f"Unexpected audio endpoint data: {name}")
            require("receiver stopped; all session threads joined" in log, "Unjoined session threads")
            require(hashlib.sha256(executable.read_bytes()).hexdigest() == executable_hash, "Executable changed during validation")
            result = {"schema_version": 2, "executable": str(executable), "sha256": executable_hash,
                      "fixture_sha256": hashlib.sha256(FIXTURE.read_bytes()).hexdigest(), "known_answer_vectors": vector_count,
                      "checks": checks, "positive_record_counts": record_counts, "audio_control": audio_control,
                      "audio_samples": 0, "audio_silence_frames": 0,
                      "evidence": "Independent Python SRP M4 verification and ChaCha20-Poly1305 loopback control against the real receiver, including audio SETUP and isolated decoder startup. No RTP audio, sync/retransmit audio packets, encoded media payload, PCM, real phone, discovery, GPU picture or A/V acceptance. Fragmented client writes may be coalesced by TCP."}
            (data / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            print(f"PASS: {data / 'result.json'}")
        finally:
            if child.poll() is None:
                child.kill(); child.wait(timeout=5)  # Only this invocation's hidden child.


if __name__ == "__main__":
    main()
