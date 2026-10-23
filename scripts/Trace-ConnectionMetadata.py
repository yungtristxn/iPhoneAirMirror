"""Bounded connection metadata trace for the explicitly selected PC/iPhone pair.

Never writes a pcap, TXT values, media/control payloads or unrelated DNS names.
Requires the already installed TShark/Npcap; changes no Windows settings.
"""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import ipaddress
import json
from pathlib import Path
import queue
import re
import stat
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
TSHARK = Path("C:/Program Files/Wireshark/tshark.exe")
MULTICAST = "224.0.0.251"
PRIVATE_NETWORKS = tuple(ipaddress.IPv4Network(value) for value in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))
MAX_LINE, MAX_OUTPUT = 65536, 256 * 1024
FIELDS = ["frame.time_epoch", "ip.src", "ip.dst", "udp.srcport", "udp.dstport",
          "tcp.srcport", "tcp.dstport", "tcp.flags", "tcp.len", "dns.flags.response",
          "dns.qry.name", "dns.qry.type", "dns.qry.class", "dns.qry.qu",
          "dns.resp.name", "dns.resp.type", "dns.resp.ttl", "dns.resp.cache_flush",
          "dns.a", "dns.aaaa", "dns.srv.target", "dns.srv.port", "dns.ptr.domain_name",
          "dns.nsec.next_domain_name"]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def private_ipv4(value):
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError as error:
        raise argparse.ArgumentTypeError("Expected an RFC1918 IPv4 address") from error
    if not any(address in network for network in PRIVATE_NETWORKS):
        raise argparse.ArgumentTypeError("Expected an RFC1918 IPv4 address")
    return str(address)


def npf_interface(value):
    if not re.fullmatch(r"\\Device\\NPF_\{[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\}", value):
        raise argparse.ArgumentTypeError(r"Use the observed \Device\NPF_{GUID} interface identifier")
    return value


def receiver_host(value):
    host = value.lower().removesuffix(".")
    labels = host.split(".")
    if (len(host) > 253 or len(labels) < 2 or labels[-1] != "local"
            or any(not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", label) for label in labels)):
        raise argparse.ArgumentTypeError("Expected a valid receiver hostname ending in .local")
    return host


def service_name(value):
    if not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9 _-]{0,61}[A-Za-z0-9])?", value):
        raise argparse.ArgumentTypeError("Service name must be 1-63 ASCII letters/digits/spaces/hyphens/underscores, with letters/digits at both ends")
    return value


def parse_cli(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pc", required=True, type=private_ipv4)
    parser.add_argument("--phone", required=True, type=private_ipv4)
    parser.add_argument("--interface", required=True, type=npf_interface)
    parser.add_argument("--receiver-host", required=True, type=receiver_host)
    parser.add_argument("--service-name", type=service_name, default="iPhone Air Mirror")
    parser.add_argument("--seconds", type=int, default=90)
    parser.add_argument("--packets", type=int, default=300)
    args = parser.parse_args(argv)
    if args.pc == args.phone:
        parser.error("--pc and --phone must be distinct")
    if not 1 <= args.seconds <= 90 or not 1 <= args.packets <= 300:
        parser.error("Capture bounds are 1-90 seconds and 1-300 packets")
    return args


def capture_filter(scope):
    # Reject TCP packets carrying any data, even when SYN/FIN/RST is also set.
    return (
        f"ip and ((udp port 5353 and (src host {scope.pc} or src host {scope.phone}) "
        f"and (dst host {scope.pc} or dst host {scope.phone} or dst host {MULTICAST})) or "
        f"(tcp port 7000 and ((src host {scope.pc} and dst host {scope.phone}) or "
        f"(src host {scope.phone} and dst host {scope.pc})) and (tcp[13] & 0x07 != 0) and "
        "(ip[2:2] = ((ip[0] & 0x0f) << 2) + ((tcp[12] & 0xf0) >> 2))))"
    )


def tshark_command(scope):
    command = [str(TSHARK), "-n", "-p", "-l", "-q", "-i", scope.interface, "-f", capture_filter(scope),
               "-s", "9000", "-a", f"duration:{scope.seconds}", "-c", str(scope.packets),
               "-T", "fields", "-E", "separator=/t", "-E", "aggregator=|", "-E", "quote=d"]
    for field in FIELDS:
        command.extend(["-e", field])
    return command


def clock_anchor():
    before = time.perf_counter()
    wall = time.time()
    after = time.perf_counter()
    perf = (before + after) / 2
    return {"wall_epoch_seconds": wall, "perf_counter_seconds": perf,
            "wall_minus_perf_seconds": wall - perf, "sample_span_seconds": after - before}


def number(text, maximum=0xffffffff):
    if not re.fullmatch(r"(?:[0-9]{1,10}|0x[0-9a-fA-F]{1,8})", text):
        return None
    value = int(text, 16 if text.startswith("0x") else 10)
    return value if value <= maximum else None


def boolean(text):
    # TShark 4.6 emits True/False for these FT_BOOLEAN fields. Keep False rather
    # than dropping it as a missing numeric value; accept numeric exports too.
    value = text.lower()
    if value in ("true", "1", "0x1", "0x01"):
        return True
    if value in ("false", "0", "0x0", "0x00"):
        return False
    return None


def name_role(text, scope):
    name = text.lower().rstrip(".").replace(r"\032", " ").replace(r"\x20", " ")
    names = {scope.receiver_host: "receiver_host", "_airplay._tcp.local": "airplay_browse",
             "_raop._tcp.local": "raop_browse",
             scope.service_name.lower() + "._airplay._tcp.local": "receiver_airplay"}
    if name in names:
        return names[name]
    if re.fullmatch(r"[0-9a-f]{12}@" + re.escape(scope.service_name.lower()) + r"\._raop\._tcp\.local", name):
        return "receiver_raop"
    return None


def address_role(text, family, scope):
    try:
        address = ipaddress.ip_address(text)
    except ValueError:
        return None
    if address.version != family:
        return None
    if family == 4:
        return {scope.pc: "pc", scope.phone: "phone"}.get(str(address), "other_ipv4")
    # Do not retain an unapproved interface address or expose another device.
    return "ipv6_link_local" if address.is_link_local else "ipv6_global" if address.is_global else "ipv6_other"


def normalized_dns(fields, scope):
    values = {key: value.split("|") if value else [] for key, value in fields.items()}
    questions, answers = [], []
    query_names, query_types = values["dns.qry.name"], values["dns.qry.type"]
    if len(query_names) == len(query_types) and len(query_names) <= 64:
        for index, (raw_name, raw_type) in enumerate(zip(query_names, query_types)):
            name, kind = name_role(raw_name, scope), number(raw_type, 65535)
            if name is None or kind is None:
                continue
            entry = {"name": name, "type": kind}
            for field, label, bound in [("dns.qry.class", "class", 65535), ("dns.qry.qu", "unicast_response", 1)]:
                if len(values[field]) == len(query_names):
                    value = boolean(values[field][index]) if field == "dns.qry.qu" else number(values[field][index], bound)
                    if value is not None:
                        entry[label] = value
            questions.append(entry)
    names, types = values["dns.resp.name"], values["dns.resp.type"]
    if len(names) == len(types) and len(names) <= 64:
        kinds = [number(value, 65535) for value in types]
        indices = {1: 0, 28: 0, 33: 0, 12: 0, 47: 0}
        extras = {1: "dns.a", 28: "dns.aaaa", 33: "dns.srv.target", 12: "dns.ptr.domain_name", 47: "dns.nsec.next_domain_name"}
        for index, (raw_name, kind) in enumerate(zip(names, kinds)):
            extra_index = indices.get(kind, 0)
            if kind in indices:
                indices[kind] += 1
            name = name_role(raw_name, scope)
            if name is None or kind is None:
                continue
            entry = {"name": name, "type": kind}
            for field, label, bound in [("dns.resp.ttl", "ttl", 0xffffffff), ("dns.resp.cache_flush", "cache_flush", 1)]:
                if len(values[field]) == len(names):
                    value = boolean(values[field][index]) if field == "dns.resp.cache_flush" else number(values[field][index], bound)
                    if value is not None:
                        entry[label] = value
            # Associate occurrence lists only if their exact RR counts agree.
            field = extras.get(kind)
            if field and len(values[field]) == kinds.count(kind):
                raw_value = values[field][extra_index]
                if kind in (1, 28):
                    role = address_role(raw_value, 4 if kind == 1 else 6, scope)
                    if role:
                        entry["address_role"] = role
                else:
                    role = name_role(raw_value, scope)
                    if role:
                        entry["target"] = role
                if kind == 33 and len(values["dns.srv.port"]) == kinds.count(33):
                    port = number(values["dns.srv.port"][extra_index], 65535)
                    if port is not None:
                        entry["port"] = port
            answers.append(entry)
    return questions[:8], answers[:16]


def normalize(row, anchor, scope):
    if len(row) != len(FIELDS):
        return None
    fields = dict(zip(FIELDS, row))
    if not re.fullmatch(r"[0-9]{1,12}(?:\.[0-9]{1,12})?", fields["frame.time_epoch"]):
        return None
    stamp = float(fields["frame.time_epoch"])
    if abs(stamp - anchor["wall_epoch_seconds"]) > 120:
        return None
    roles = {scope.pc: "pc", scope.phone: "phone", MULTICAST: "mdns_multicast"}
    source, destination = roles.get(fields["ip.src"]), roles.get(fields["ip.dst"])
    if source not in ("pc", "phone") or destination is None:
        return None
    result = {"unix_seconds": stamp, "perf_counter_estimate": stamp - anchor["wall_minus_perf_seconds"],
              "source": source, "destination": destination}
    if fields["tcp.flags"]:
        flags = number(fields["tcp.flags"], 0x1ff)
        ports = [number(fields[key], 65535) for key in ("tcp.srcport", "tcp.dstport")]
        if (flags is None or not flags & 7 or fields["tcp.len"] != "0" or 7000 not in ports
                or destination == "mdns_multicast" or source == destination):
            return None
        result.update(event="tcp_flags", flags=[name for bit, name in [(1, "FIN"), (2, "SYN"), (4, "RST"), (16, "ACK")] if flags & bit])
        return result
    ports = [number(fields[key], 65535) for key in ("udp.srcport", "udp.dstport")]
    if None in ports or 5353 not in ports:
        return None
    questions, answers = normalized_dns(fields, scope)
    if not questions and not answers:
        return None
    result.update(event="mdns", source_port=ports[0], destination_port=ports[1],
                  response=boolean(fields["dns.flags.response"]), questions=questions, records=answers)
    return result


def output_directory():
    target = ROOT / "local/connection-metadata" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    for path in [ROOT, ROOT / "local", target.parent]:
        if path.exists():
            require(not path.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT, "Output parent must not be a directory link")
    require(target.resolve().is_relative_to(ROOT.resolve() / "local"), "Output must stay under workspace local")
    target.mkdir(parents=True, exist_ok=False)
    return target


def main():
    args = parse_cli()
    require(TSHARK.is_file() and TSHARK.resolve().name.lower() == "tshark.exe", "Installed TShark executable is missing")
    command = tshark_command(args)
    folder = output_directory()
    anchor = clock_anchor()
    rows = queue.Queue(maxsize=64)
    stats = {"field_lines": 0, "queue_dropped": 0, "oversized_lines": 0, "stderr_lines": 0}
    diagnostics = set()
    child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             creationflags=subprocess.CREATE_NO_WINDOW, shell=False)

    def consume(stream, metadata):
        while True:
            line = stream.readline(MAX_LINE + 1)
            if not line:
                return
            if len(line) > MAX_LINE or not line.endswith(b"\n"):
                stats["oversized_lines"] += 1
                return
            if metadata:
                stats["field_lines"] += 1
                try:
                    rows.put_nowait(line)
                except queue.Full:
                    stats["queue_dropped"] += 1
            else:
                stats["stderr_lines"] += 1
                text = line.lower()
                for needle, label in [(b"permission", "permission"), (b"access denied", "permission"),
                                      (b"capture filter", "capture_filter"), (b"invalid field", "fields"),
                                      (b"no such device", "interface"), (b"npcap", "npcap")]:
                    if needle in text:
                        diagnostics.add(label)
                # Never store or print raw stderr, DNS fields or packet text.

    readers = [threading.Thread(target=consume, args=(child.stdout, True), daemon=True),
               threading.Thread(target=consume, args=(child.stderr, False), daemon=True)]
    for reader in readers:
        reader.start()
    deadline = time.monotonic() + args.seconds + 3
    count, filtered, written = 0, 0, 0
    counts, first_seen, last_seen = {}, {}, {}
    reason = "tshark_finished"
    try:
        with (folder / "metadata.jsonl").open("x", encoding="utf-8") as output:
            while child.poll() is None or readers[0].is_alive() or not rows.empty():
                if time.monotonic() >= deadline or stats["oversized_lines"]:
                    reason = "deadline" if not stats["oversized_lines"] else "oversized_field_line"
                    break
                try:
                    line = rows.get(timeout=.1)
                except queue.Empty:
                    continue
                try:
                    row = next(csv.reader([line.decode("utf-8")], delimiter="\t", strict=True))
                    event = normalize(row, anchor, args)
                except (UnicodeError, csv.Error, ValueError):
                    event = None
                if event is None:
                    filtered += 1
                    continue
                encoded = json.dumps(event, separators=(",", ":")) + "\n"
                size = len(encoded.encode("utf-8"))
                if count >= args.packets or written + size > MAX_OUTPUT:
                    reason = "sanitized_output_budget"
                    break
                output.write(encoded); output.flush()
                count += 1; written += size
                category = event["event"] + ":" + event["source"]
                counts[category] = counts.get(category, 0) + 1
                first_seen.setdefault(category, event["unix_seconds"])
                last_seen[category] = event["unix_seconds"]
    finally:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                child.kill(); child.wait(timeout=2)
        for reader in readers:
            reader.join(timeout=.5)
        child.stdout.close(); child.stderr.close()
        end = clock_anchor()
        summary = {"schema_version": 2, "scope": {"pc": args.pc, "phone": args.phone, "interface": args.interface,
                                                    "receiver_host": args.receiver_host, "service_name": args.service_name},
                   "capture_filter": capture_filter(args), "maximum_seconds": args.seconds, "maximum_packets": args.packets,
                   "start_clock": anchor, "end_clock": end,
                   "clock_offset_change_ms": (end["wall_minus_perf_seconds"] - anchor["wall_minus_perf_seconds"]) * 1000,
                   "exit_code": child.returncode, "stop_reason": reason, "emitted_packets": count,
                   "filtered_packets": filtered, "jsonl_bytes": written, **stats,
                   "diagnostic_categories": sorted(diagnostics), "counts": counts,
                   "first_seen_unix": first_seen, "last_seen_unix": last_seen,
                   "evidence": "Only scoped mDNS metadata and zero-data TCP7000 SYN/FIN/RST packets. DNS names become fixed receiver/browse roles; IPv4 answers become roles and IPv6 answers scope labels. No raw capture, TXT values or media/control payload stored. NSEC presence does not establish a negative AAAA answer. These timestamps do not measure the user's tap or end-to-end picture latency."}
        (folder / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"Connection metadata: {folder / 'summary.json'}")
    require(child.returncode == 0 and reason == "tshark_finished" and not stats["oversized_lines"],
            "Capture did not finish normally; see sanitized summary")


if __name__ == "__main__":
    main()
