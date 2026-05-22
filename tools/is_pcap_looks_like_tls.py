#!/usr/bin/env python3

import argparse
import ctypes
import ctypes.util
import ipaddress
import json
import struct
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


ALLOWED_TLS_CONTENT_TYPES = {20, 21, 22, 23}
TLS_APPLICATION_DATA = 23
MAX_TLS_RECORD_LENGTH = 18432

DLT_EN10MB = 1
DLT_RAW = 101
DLT_LINUX_SLL = 113
DLT_LINUX_SLL2 = 276


class TimeVal(ctypes.Structure):
    _fields_ = [
        ("tv_sec", ctypes.c_long),
        ("tv_usec", ctypes.c_long),
    ]


class PcapPkthdr(ctypes.Structure):
    _fields_ = [
        ("ts", TimeVal),
        ("caplen", ctypes.c_uint32),
        ("len", ctypes.c_uint32),
    ]


@dataclass(frozen=True)
class Endpoint:
    host: str
    port: int


@dataclass(frozen=True)
class TcpPacket:
    src: Endpoint
    dst: Endpoint
    seq: int
    payload: bytes


@dataclass
class TlsRecordSummary:
    content_type: int
    version: str
    length: int


class PcapShapeError(RuntimeError):
    pass


class LibpcapReader:
    def __init__(self):
        library = ctypes.util.find_library("pcap")
        if library is None:
            raise PcapShapeError("libpcap is not available")
        self.lib = ctypes.CDLL(library)
        self.lib.pcap_open_offline.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        self.lib.pcap_open_offline.restype = ctypes.c_void_p
        self.lib.pcap_close.argtypes = [ctypes.c_void_p]
        self.lib.pcap_close.restype = None
        self.lib.pcap_datalink.argtypes = [ctypes.c_void_p]
        self.lib.pcap_datalink.restype = ctypes.c_int
        self.lib.pcap_geterr.argtypes = [ctypes.c_void_p]
        self.lib.pcap_geterr.restype = ctypes.c_char_p
        self.lib.pcap_next_ex.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.POINTER(PcapPkthdr)),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)),
        ]
        self.lib.pcap_next_ex.restype = ctypes.c_int

    def read_packets(self, path):
        errbuf = ctypes.create_string_buffer(256)
        handle = self.lib.pcap_open_offline(str(path).encode("utf-8"), errbuf)
        if not handle:
            message = errbuf.value.decode("utf-8", errors="replace")
            raise PcapShapeError(f"failed to open pcap: {message}")
        try:
            linktype = self.lib.pcap_datalink(handle)
            packets = []
            header_ptr = ctypes.POINTER(PcapPkthdr)()
            data_ptr = ctypes.POINTER(ctypes.c_ubyte)()
            while True:
                rc = self.lib.pcap_next_ex(
                    handle,
                    ctypes.byref(header_ptr),
                    ctypes.byref(data_ptr),
                )
                if rc == 1:
                    header = header_ptr.contents
                    packet = ctypes.string_at(data_ptr, header.caplen)
                    parsed = parse_link_packet(linktype, packet)
                    if parsed is not None:
                        packets.append(parsed)
                    continue
                if rc == -2:
                    break
                if rc == 0:
                    continue
                error = self.lib.pcap_geterr(handle)
                message = error.decode("utf-8", errors="replace") if error else "unknown"
                raise PcapShapeError(f"libpcap read failed: {message}")
            return packets
        finally:
            self.lib.pcap_close(handle)


def parse_endpoint(text):
    if ":" not in text:
        raise argparse.ArgumentTypeError("endpoint must be HOST:PORT")
    host, port_text = text.rsplit(":", 1)
    try:
        port = int(port_text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("endpoint port must be numeric") from exc
    if port <= 0 or port > 65535:
        raise argparse.ArgumentTypeError("endpoint port out of range")
    return Endpoint(host, port)


def parse_link_packet(linktype, packet):
    if linktype == DLT_EN10MB:
        return parse_ethernet(packet)
    if linktype == DLT_RAW:
        return parse_ip_packet(packet)
    if linktype == DLT_LINUX_SLL:
        if len(packet) < 16:
            return None
        protocol = struct.unpack("!H", packet[14:16])[0]
        return parse_link_payload(protocol, packet[16:])
    if linktype == DLT_LINUX_SLL2:
        if len(packet) < 20:
            return None
        protocol = struct.unpack("!H", packet[0:2])[0]
        return parse_link_payload(protocol, packet[20:])
    raise PcapShapeError(f"unsupported pcap linktype {linktype}")


def parse_ethernet(packet):
    if len(packet) < 14:
        return None
    offset = 14
    ethertype = struct.unpack("!H", packet[12:14])[0]
    while ethertype in (0x8100, 0x88A8):
        if len(packet) < offset + 4:
            return None
        ethertype = struct.unpack("!H", packet[offset + 2 : offset + 4])[0]
        offset += 4
    return parse_link_payload(ethertype, packet[offset:])


def parse_link_payload(protocol, payload):
    if protocol == 0x0800:
        return parse_ipv4(payload)
    if protocol == 0x86DD:
        return parse_ipv6(payload)
    return None


def parse_ip_packet(payload):
    if not payload:
        return None
    version = payload[0] >> 4
    if version == 4:
        return parse_ipv4(payload)
    if version == 6:
        return parse_ipv6(payload)
    return None


def parse_ipv4(payload):
    if len(payload) < 20:
        return None
    ihl = (payload[0] & 0x0F) * 4
    if ihl < 20 or len(payload) < ihl:
        return None
    total_length = struct.unpack("!H", payload[2:4])[0]
    if total_length < ihl or len(payload) < total_length:
        return None
    flags_fragment = struct.unpack("!H", payload[6:8])[0]
    if flags_fragment & 0x1FFF:
        return None
    protocol = payload[9]
    if protocol != 6:
        return None
    src = str(ipaddress.IPv4Address(payload[12:16]))
    dst = str(ipaddress.IPv4Address(payload[16:20]))
    return parse_tcp(src, dst, payload[ihl:total_length])


def parse_ipv6(payload):
    if len(payload) < 40:
        return None
    payload_length = struct.unpack("!H", payload[4:6])[0]
    next_header = payload[6]
    if next_header != 6:
        return None
    total_length = 40 + payload_length
    if len(payload) < total_length:
        return None
    src = str(ipaddress.IPv6Address(payload[8:24]))
    dst = str(ipaddress.IPv6Address(payload[24:40]))
    return parse_tcp(src, dst, payload[40:total_length])


def parse_tcp(src_host, dst_host, payload):
    if len(payload) < 20:
        return None
    src_port, dst_port = struct.unpack("!HH", payload[:4])
    seq = struct.unpack("!I", payload[4:8])[0]
    data_offset = (payload[12] >> 4) * 4
    if data_offset < 20 or len(payload) < data_offset:
        return None
    data = payload[data_offset:]
    if not data:
        return None
    return TcpPacket(
        src=Endpoint(src_host, src_port),
        dst=Endpoint(dst_host, dst_port),
        seq=seq,
        payload=data,
    )


def packet_matches(packet, port=None, endpoint_a=None, endpoint_b=None):
    if port is not None and packet.src.port != port and packet.dst.port != port:
        return False
    if endpoint_a is None or endpoint_b is None:
        return True
    return (
        packet.src == endpoint_a
        and packet.dst == endpoint_b
        or packet.src == endpoint_b
        and packet.dst == endpoint_a
    )


def reassemble_direction(segments):
    if not segments:
        return b""
    ordered = sorted(segments, key=lambda item: item[0])
    expected = ordered[0][0]
    out = bytearray()
    for seq, payload in ordered:
        end = seq + len(payload)
        if end <= expected:
            continue
        if seq < expected:
            payload = payload[expected - seq :]
            seq = expected
        if seq != expected:
            raise PcapShapeError(f"tcp sequence gap: expected {expected}, got {seq}")
        out.extend(payload)
        expected = seq + len(payload)
    return bytes(out)


def parse_tls_records(stream_bytes):
    records = []
    offset = 0
    while offset < len(stream_bytes):
        if len(stream_bytes) - offset < 5:
            raise PcapShapeError(
                f"trailing {len(stream_bytes) - offset} byte(s) after TLS records"
            )
        content_type = stream_bytes[offset]
        major = stream_bytes[offset + 1]
        minor = stream_bytes[offset + 2]
        length = struct.unpack("!H", stream_bytes[offset + 3 : offset + 5])[0]
        if content_type not in ALLOWED_TLS_CONTENT_TYPES:
            raise PcapShapeError(f"non-TLS content type {content_type} at offset {offset}")
        if major != 3 or minor > 4:
            raise PcapShapeError(
                f"unexpected TLS record version {major}.{minor} at offset {offset}"
            )
        if length > MAX_TLS_RECORD_LENGTH:
            raise PcapShapeError(f"oversized TLS record length {length} at offset {offset}")
        next_offset = offset + 5 + length
        if next_offset > len(stream_bytes):
            raise PcapShapeError(f"truncated TLS record at offset {offset}")
        records.append(
            TlsRecordSummary(
                content_type=content_type,
                version=f"{major}.{minor}",
                length=length,
            )
        )
        offset = next_offset
    return records


def is_pcap_looks_like_tls(
    pcap_path,
    filter_by_src_dst=None,
    port=None,
    require_bidirectional=False,
    require_application_data=False,
    min_records=1,
):
    endpoint_a = None
    endpoint_b = None
    if filter_by_src_dst is not None:
        endpoint_a, endpoint_b = filter_by_src_dst

    packets = [
        packet
        for packet in LibpcapReader().read_packets(pcap_path)
        if packet_matches(packet, port=port, endpoint_a=endpoint_a, endpoint_b=endpoint_b)
    ]
    if not packets:
        raise PcapShapeError("no TCP payload packets matched the filter")

    segments = defaultdict(list)
    for packet in packets:
        key = (packet.src, packet.dst)
        segments[key].append((packet.seq, packet.payload))

    streams = []
    for (src, dst), direction_segments in sorted(
        segments.items(), key=lambda item: (item[0][0].host, item[0][0].port, item[0][1].host, item[0][1].port)
    ):
        stream_bytes = reassemble_direction(direction_segments)
        if not stream_bytes:
            continue
        records = parse_tls_records(stream_bytes)
        if records:
            streams.append({"src": src, "dst": dst, "records": records})

    if not streams:
        raise PcapShapeError("no TLS records found in matched TCP payloads")
    if require_bidirectional and len(streams) < 2:
        raise PcapShapeError("expected TLS records in both directions")

    total_records = sum(len(stream["records"]) for stream in streams)
    if total_records < min_records:
        raise PcapShapeError(f"expected at least {min_records} TLS records, got {total_records}")
    if require_application_data and not any(
        record.content_type == TLS_APPLICATION_DATA
        for stream in streams
        for record in stream["records"]
    ):
        raise PcapShapeError("expected at least one TLS Application Data record")

    return {
        "pcap": str(pcap_path),
        "streams": streams,
        "total_records": total_records,
    }


def serializable_summary(summary):
    return {
        "pcap": summary["pcap"],
        "total_records": summary["total_records"],
        "streams": [
            {
                "src": f"{stream['src'].host}:{stream['src'].port}",
                "dst": f"{stream['dst'].host}:{stream['dst'].port}",
                "record_count": len(stream["records"]),
                "application_data_count": sum(
                    1 for record in stream["records"] if record.content_type == TLS_APPLICATION_DATA
                ),
                "content_types": sorted({record.content_type for record in stream["records"]}),
            }
            for stream in summary["streams"]
        ],
    }


def main():
    parser = argparse.ArgumentParser(
        description="Validate that selected TCP payloads in a pcap parse as TLS records."
    )
    parser.add_argument("pcap")
    parser.add_argument("--port", type=int, help="match TCP packets with this source or destination port")
    parser.add_argument("--src", type=parse_endpoint, help="source endpoint for exact src/dst filter")
    parser.add_argument("--dst", type=parse_endpoint, help="destination endpoint for exact src/dst filter")
    parser.add_argument("--require-bidirectional", action="store_true")
    parser.add_argument("--require-application-data", action="store_true")
    parser.add_argument("--min-records", type=int, default=1)
    parser.add_argument("--summary", help="write JSON summary to this path")
    args = parser.parse_args()

    if (args.src is None) != (args.dst is None):
        parser.error("--src and --dst must be provided together")
    if args.port is not None and (args.port <= 0 or args.port > 65535):
        parser.error("--port out of range")
    if args.min_records <= 0:
        parser.error("--min-records must be positive")

    filter_by_src_dst = None
    if args.src is not None:
        filter_by_src_dst = (args.src, args.dst)

    try:
        summary = is_pcap_looks_like_tls(
            args.pcap,
            filter_by_src_dst=filter_by_src_dst,
            port=args.port,
            require_bidirectional=args.require_bidirectional,
            require_application_data=args.require_application_data,
            min_records=args.min_records,
        )
    except PcapShapeError as error:
        print(f"pcap does not look like TLS: {error}", file=sys.stderr)
        return 1

    output = serializable_summary(summary)
    text = json.dumps(output, indent=2, sort_keys=True)
    if args.summary:
        Path(args.summary).write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
