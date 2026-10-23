"""Pure metadata fixtures: no subprocesses, sockets, packet capture or phone data."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("connection_metadata", ROOT / "scripts/Trace-ConnectionMetadata.py")
trace = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(trace)

BASE = ["--pc", "10.23.45.6", "--phone", "10.23.45.7", "--interface",
        r"\Device\NPF_{00000000-0000-4000-8000-000000000001}", "--receiver-host", "ScopeBox.local"]
ANCHOR = {"wall_epoch_seconds": 1700000000.0, "wall_minus_perf_seconds": 1699999000.0}


def scope(custom=True):
    return trace.parse_cli(BASE + (["--service-name", "Scope HAP"] if custom else []))


def fields(**overrides):
    result = dict.fromkeys(trace.FIELDS, "")
    result.update({"frame.time_epoch": "1700000001.250", "ip.src": "10.23.45.7", "ip.dst": trace.MULTICAST,
                   "udp.srcport": "5353", "udp.dstport": "5353", "dns.flags.response": "False",
                   "dns.qry.name": "ScopeBox.local", "dns.qry.type": "28", "dns.qry.class": "1", "dns.qry.qu": "False"})
    result.update(overrides)
    return result


def normalize(values):
    return trace.normalize([values[name] for name in trace.FIELDS], ANCHOR, scope())


class MetadataTests(unittest.TestCase):
    def test_boolean_spellings_and_unknown_values(self):
        for value in ("True", "true", "TRUE", "1", "0x1", "0x01"):
            self.assertIs(trace.boolean(value), True)
        for value in ("False", "false", "FALSE", "0", "0x0", "0x00"):
            self.assertIs(trace.boolean(value), False)
        for value in ("", "yes", "2", "True|False", "True ", "-1"):
            self.assertIsNone(trace.boolean(value))

    def test_qr_qu_and_cache_flush_preserve_both_boolean_values(self):
        for value, expected in (("False", False), ("True", True), ("0", False), ("1", True)):
            event = normalize(fields(**{"dns.flags.response": value, "dns.qry.qu": value,
                                        "dns.resp.name": "ScopeBox.local", "dns.resp.type": "1", "dns.a": "10.23.45.6",
                                        "dns.resp.ttl": "120", "dns.resp.cache_flush": value}))
            self.assertIs(event["response"], expected)
            self.assertIs(event["questions"][0]["unicast_response"], expected)
            self.assertIs(event["records"][0]["cache_flush"], expected)
            self.assertEqual(event["records"][0]["ttl"], 120)
            self.assertEqual(event["questions"][0]["class"], 1)

    def test_normalization_discards_unrelated_names_and_address_values(self):
        event = normalize(fields(**{
            "dns.qry.name": "Secret-Printer.local|_airplay._tcp.local", "dns.qry.type": "1|12",
            "dns.qry.class": "1|1", "dns.qry.qu": "False|True", "dns.flags.response": "True",
            "dns.resp.name": "ScopeBox.local|Secret-Printer.local|Scope HAP._airplay._tcp.local|_airplay._tcp.local|ScopeBox.local|_raop._tcp.local|ScopeBox.local",
            "dns.resp.type": "1|1|33|12|47|12|28", "dns.resp.ttl": "120|120|120|120|120|120|120",
            "dns.resp.cache_flush": "True|True|True|False|True|False|True",
            "dns.a": "10.23.45.6|10.99.88.77", "dns.aaaa": "fe80::1234",
            "dns.srv.target": "ScopeBox.local", "dns.srv.port": "7000",
            "dns.ptr.domain_name": "Scope HAP._airplay._tcp.local|0123456789ab@Scope HAP._raop._tcp.local",
            "dns.nsec.next_domain_name": "ScopeBox.local"}))
        encoded = json.dumps(event)
        for private in ("Secret-Printer", "10.99.88.77", "ScopeBox", "Scope HAP", "0123456789ab", "fe80::1234"):
            self.assertNotIn(private, encoded)
        self.assertEqual(event["questions"], [{"name": "airplay_browse", "type": 12, "class": 1, "unicast_response": True}])
        self.assertEqual(len(event["records"]), 6)
        self.assertIn({"name": "receiver_host", "type": 47, "ttl": 120, "cache_flush": True, "target": "receiver_host"}, event["records"])
        self.assertIn("ipv6_link_local", encoded)
        self.assertNotIn("negative_aaaa", encoded)
        self.assertFalse(any(field.startswith("dns.txt") for field in trace.FIELDS))
        self.assertIsNone(normalize(fields(**{"dns.qry.name": "Secret-Printer.local"})))

    def test_mismatched_record_occurrences_do_not_invent_address_associations(self):
        event = normalize(fields(**{"dns.resp.name": "ScopeBox.local|Secret-Printer.local", "dns.resp.type": "1|1",
                                    "dns.a": "10.99.88.77", "dns.resp.ttl": "120", "dns.resp.cache_flush": "True"}))
        self.assertEqual(event["records"], [{"name": "receiver_host", "type": 1}])

    def test_tcp_requires_selected_pair_flags_port_and_no_data(self):
        tcp = fields(**{"ip.dst": "10.23.45.6", "udp.srcport": "", "udp.dstport": "",
                        "tcp.srcport": "42000", "tcp.dstport": "7000", "tcp.flags": "0x0002", "tcp.len": "0"})
        event = normalize(tcp)
        self.assertEqual(event["flags"], ["SYN"])
        self.assertEqual(event["perf_counter_estimate"], 1001.25)
        for override in ({"tcp.len": "1"}, {"tcp.flags": "0x0010"}, {"tcp.dstport": "7001"},
                         {"ip.src": "10.23.45.8"}, {"ip.src": "10.23.45.6"}, {"ip.dst": trace.MULTICAST}):
            self.assertIsNone(normalize({**tcp, **override}))
        event = normalize(fields(**{"udp.srcport": "42001"}))
        self.assertEqual(event["source_port"], 42001)

    def test_cli_scope_and_capture_command_are_explicit_and_bounded(self):
        settings = scope(False)
        self.assertEqual((settings.seconds, settings.packets, settings.service_name), (90, 300, "iPhone Air Mirror"))
        command = trace.tshark_command(settings)
        self.assertIn("-n", command)
        self.assertIn("-p", command)
        self.assertNotIn("-w", command)
        self.assertEqual(command[command.index("-i") + 1], settings.interface)
        self.assertEqual(command[command.index("-a") + 1], "duration:90")
        self.assertEqual(command[command.index("-c") + 1], "300")
        bpf = command[command.index("-f") + 1]
        self.assertIn("src host 10.23.45.6", bpf)
        self.assertIn("dst host 10.23.45.7", bpf)
        self.assertIn("tcp[13] & 0x07 != 0", bpf)
        self.assertIn("ip[2:2] = ((ip[0] & 0x0f) << 2) + ((tcp[12] & 0xf0) >> 2)", bpf)
        self.assertEqual(trace.name_role("SCOPEBOX.LOCAL.", settings), "receiver_host")
        self.assertIsNone(trace.name_role("Scope HAP._airplay._tcp.local", settings))
        self.assertEqual(trace.name_role("Scope HAP._airplay._tcp.local", scope()), "receiver_airplay")

    def test_invalid_cli_values_are_rejected_before_any_capture(self):
        invalid = [("--pc", value) for value in ("8.8.8.8", "127.0.0.1", "169.254.1.1", "::1", "10.1.1.1 or udp")]
        invalid += [("--phone", "10.23.45.6"), ("--interface", "1"), ("--interface", r"\Device\NPF_{not-a-guid}"),
                    ("--receiver-host", "example.com"), ("--receiver-host", "bad..local"), ("--receiver-host", "*.local"),
                    ("--service-name", "A|B"), ("--service-name", "A.B"), ("--service-name", "a" * 64),
                    ("--seconds", "0"), ("--seconds", "91"), ("--packets", "0"), ("--packets", "301")]
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                trace.parse_cli([])
            for option, value in invalid:
                with self.subTest(option=option, value=value), self.assertRaises(SystemExit) as error:
                    trace.parse_cli(BASE + [option, value])
                self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
