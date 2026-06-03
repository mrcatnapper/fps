#!/usr/bin/env python3

import argparse
import ctypes.util
import ipaddress
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


SKIP = 77


def skip(reason):
    print(f"SKIP: {reason}", file=sys.stderr)
    return SKIP


def tls_record(payload_size):
    return bytes([23, 3, 3]) + struct.pack("!H", payload_size) + bytes([payload_size % 251]) * payload_size


def tcp_packet(src, dst, src_port, dst_port, seq, ack, flags, payload=b""):
    src_ip = ipaddress.IPv4Address(src).packed
    dst_ip = ipaddress.IPv4Address(dst).packed
    tcp_header = struct.pack(
        "!HHIIBBHHH",
        src_port,
        dst_port,
        seq,
        ack,
        5 << 4,
        flags,
        65535,
        0,
        0,
    )
    ip_total = 20 + len(tcp_header) + len(payload)
    ip_header = struct.pack(
        "!BBHHHBBH4s4s",
        0x45,
        0,
        ip_total,
        1,
        0,
        64,
        6,
        0,
        src_ip,
        dst_ip,
    )
    ethernet = b"\x02\x00\x00\x00\x00\x01" + b"\x02\x00\x00\x00\x00\x02" + struct.pack("!H", 0x0800)
    return ethernet + ip_header + tcp_header + payload


def write_pcap(path):
    client_ip = "10.0.0.2"
    server_ip = "10.0.0.1"
    client_port = 40000
    server_port = 443
    client_seq = 1000
    server_seq = 5000
    packets = []

    def add(timestamp, packet):
        packets.append((timestamp, packet))

    add(1.000000, tcp_packet(client_ip, server_ip, client_port, server_port, client_seq, 0, 0x02))
    client_seq += 1
    add(1.010000, tcp_packet(server_ip, client_ip, server_port, client_port, server_seq, client_seq, 0x12))
    server_seq += 1
    add(1.020000, tcp_packet(client_ip, server_ip, client_port, server_port, client_seq, server_seq, 0x10))

    c2s_100 = tls_record(95)
    add(2.000000, tcp_packet(client_ip, server_ip, client_port, server_port, client_seq, server_seq, 0x18, c2s_100[:7]))
    client_seq += 7
    add(2.005000, tcp_packet(client_ip, server_ip, client_port, server_port, client_seq, server_seq, 0x18, c2s_100[7:]))
    client_seq += len(c2s_100) - 7

    s2c_200 = tls_record(195)
    add(2.010000, tcp_packet(server_ip, client_ip, server_port, client_port, server_seq, client_seq, 0x18, s2c_200))
    server_seq += len(s2c_200)

    c2s_128 = tls_record(123)
    add(2.025000, tcp_packet(client_ip, server_ip, client_port, server_port, client_seq, server_seq, 0x18, c2s_128))
    client_seq += len(c2s_128)

    s2c_255 = tls_record(250)
    add(2.060000, tcp_packet(server_ip, client_ip, server_port, client_port, server_seq, client_seq, 0x18, s2c_255))
    server_seq += len(s2c_255)

    c2s_69 = tls_record(64)
    add(2.080000, tcp_packet(client_ip, server_ip, client_port, server_port, client_seq, server_seq, 0x18, c2s_69))
    client_seq += len(c2s_69)

    s2c_69 = tls_record(64)
    add(2.200000, tcp_packet(server_ip, client_ip, server_port, client_port, server_seq, client_seq, 0x18, s2c_69))

    with path.open("wb") as handle:
        handle.write(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        for timestamp, packet in packets:
            seconds = int(timestamp)
            usec = int(round((timestamp - seconds) * 1_000_000.0))
            handle.write(struct.pack("<IIII", seconds, usec, len(packet), len(packet)))
            handle.write(packet)


def run(command):
    return subprocess.run(command, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=str(Path(__file__).resolve().parents[2]))
    args = parser.parse_args()

    if ctypes.util.find_library("pcap") is None:
        return skip("missing libpcap runtime")

    repo = Path(args.repo)
    shape_tool = repo / "tools" / "is_pcap_looks_like_tls.py"
    profile_tool = repo / "tools" / "pcap_to_shaper_profile.py"
    flow_tool = repo / "tools" / "analyze_pcap_tcp_flow.py"

    with tempfile.TemporaryDirectory(prefix="fps-pcap-shaper-profile-") as tmp:
        tmpdir = Path(tmp)
        pcap = tmpdir / "synthetic-carrier.pcap"
        profile_path = tmpdir / "profile.json"
        summary_path = tmpdir / "summary.json"
        flow_summary_path = tmpdir / "flow-summary.json"
        flow_csv_path = tmpdir / "flow-packets.csv"
        flow_svg_path = tmpdir / "flow.svg"
        write_pcap(pcap)

        run(
            [
                sys.executable,
                str(shape_tool),
                str(pcap),
                "--port",
                "443",
                "--require-bidirectional",
                "--require-application-data",
                "--min-records",
                "6",
            ]
        )
        run(
            [
                sys.executable,
                str(profile_tool),
                str(pcap),
                "--port",
                "443",
                "--profile-id",
                "synthetic-profile",
                "--bins",
                "3",
                "--output",
                str(profile_path),
                "--summary",
                str(summary_path),
            ]
        )
        run(
            [
                sys.executable,
                str(flow_tool),
                str(pcap),
                "--port",
                "443",
                "--split-time-epoch",
                "2.05",
                "--summary-json",
                str(flow_summary_path),
                "--packets-csv",
                str(flow_csv_path),
                "--svg",
                str(flow_svg_path),
            ]
        )

        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        flow_summary = json.loads(flow_summary_path.read_text(encoding="utf-8"))

    if profile["profile_id"] != "synthetic-profile":
        raise RuntimeError(f"unexpected profile id: {profile!r}")
    if profile["record_size_cdf_c2s"] != [[69, 1 / 3], [100, 2 / 3], [128, 1.0]]:
        raise RuntimeError(f"unexpected c2s sizes: {profile['record_size_cdf_c2s']!r}")
    if profile["record_size_cdf_s2c"] != [[69, 1 / 3], [200, 2 / 3], [255, 1.0]]:
        raise RuntimeError(f"unexpected s2c sizes: {profile['record_size_cdf_s2c']!r}")
    if profile["inter_record_delay_us_cdf_c2s"] != [[20000, 0.5], [55000, 1.0]]:
        raise RuntimeError(f"unexpected c2s delays: {profile['inter_record_delay_us_cdf_c2s']!r}")
    if profile["inter_record_delay_us_cdf_s2c"] != [[50000, 0.5], [140000, 1.0]]:
        raise RuntimeError(f"unexpected s2c delays: {profile['inter_record_delay_us_cdf_s2c']!r}")
    if summary["included_connections"] != 1 or summary["client_to_server"]["record_count"] != 3:
        raise RuntimeError(f"unexpected profile summary: {summary!r}")
    if flow_summary["all"]["packet_count"] != 10 or flow_summary["after_upgrade"]["packet_count"] != 3:
        raise RuntimeError(f"unexpected flow analyzer summary: {flow_summary!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
