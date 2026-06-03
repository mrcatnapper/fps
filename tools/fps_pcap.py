#!/usr/bin/env python3

"""Small libpcap/TCP/TLS helpers shared by FPS pcap tools.

This module intentionally has no third-party Python dependencies. It uses
libpcap through ctypes and parses only the packet shapes FPS tooling needs:
Ethernet, raw IPv4/IPv6, Linux cooked captures and TCP.
"""

import argparse
import ctypes
import ctypes.util
import ipaddress
import struct
from collections import defaultdict
from dataclasses import dataclass


ALLOWED_TLS_CONTENT_TYPES = {20, 21, 22, 23}
TLS_APPLICATION_DATA = 23
MAX_TLS_RECORD_LENGTH = 18432

DLT_EN10MB = 1
DLT_RAW = 101
DLT_LINUX_SLL = 113
DLT_LINUX_SLL2 = 276

TCP_FIN = 0x01
TCP_SYN = 0x02
TCP_RST = 0x04
TCP_ACK = 0x10


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
    timestamp: float
    src: Endpoint
    dst: Endpoint
    seq: int
    ack: int
    ip_len: int
    tcp_payload_len: int
    flags: int
    payload: bytes

    @property
    def syn(self):
        return (self.flags & TCP_SYN) != 0

    @property
    def ack_flag(self):
        return (self.flags & TCP_ACK) != 0


@dataclass(frozen=True)
class TcpPayloadSegment:
    seq: int
    timestamp: float
    payload: bytes


@dataclass
class TlsRecordSummary:
    content_type: int
    version: str
    length: int
    wire_size: int
    timestamp: float | None = None


class PcapError(RuntimeError):
    pass


class LibpcapReader:
    def __init__(self):
        library = ctypes.util.find_library("pcap")
        if library is None:
            raise PcapError("libpcap is not available")
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
            raise PcapError(f"failed to open pcap: {message}")
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
                    timestamp = float(header.ts.tv_sec) + float(header.ts.tv_usec) / 1_000_000.0
                    packet = ctypes.string_at(data_ptr, header.caplen)
                    parsed = parse_link_packet(timestamp, linktype, packet)
                    if parsed is not None:
                        packets.append(parsed)
                    continue
                if rc == -2:
                    break
                if rc == 0:
                    continue
                error = self.lib.pcap_geterr(handle)
                message = error.decode("utf-8", errors="replace") if error else "unknown"
                raise PcapError(f"libpcap read failed: {message}")
            return packets
        finally:
            self.lib.pcap_close(handle)


def parse_endpoint(text):
    host, separator, port_text = text.rpartition(":")
    if not separator or not host or not port_text:
        raise argparse.ArgumentTypeError("endpoint must be HOST:PORT")
    try:
        port = int(port_text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("endpoint port must be numeric") from error
    if port <= 0 or port > 65535:
        raise argparse.ArgumentTypeError("endpoint port out of range")
    return Endpoint(host, port)


def parse_link_packet(timestamp, linktype, packet):
    if linktype == DLT_EN10MB:
        return parse_ethernet(timestamp, packet)
    if linktype == DLT_RAW:
        return parse_ip_packet(timestamp, packet)
    if linktype == DLT_LINUX_SLL:
        if len(packet) < 16:
            return None
        protocol = struct.unpack("!H", packet[14:16])[0]
        return parse_link_payload(timestamp, protocol, packet[16:])
    if linktype == DLT_LINUX_SLL2:
        if len(packet) < 20:
            return None
        protocol = struct.unpack("!H", packet[0:2])[0]
        return parse_link_payload(timestamp, protocol, packet[20:])
    raise PcapError(f"unsupported pcap linktype {linktype}")


def parse_ethernet(timestamp, packet):
    if len(packet) < 14:
        return None
    offset = 14
    ethertype = struct.unpack("!H", packet[12:14])[0]
    while ethertype in (0x8100, 0x88A8):
        if len(packet) < offset + 4:
            return None
        ethertype = struct.unpack("!H", packet[offset + 2 : offset + 4])[0]
        offset += 4
    return parse_link_payload(timestamp, ethertype, packet[offset:])


def parse_link_payload(timestamp, protocol, payload):
    if protocol == 0x0800:
        return parse_ipv4(timestamp, payload)
    if protocol == 0x86DD:
        return parse_ipv6(timestamp, payload)
    return None


def parse_ip_packet(timestamp, payload):
    if not payload:
        return None
    version = payload[0] >> 4
    if version == 4:
        return parse_ipv4(timestamp, payload)
    if version == 6:
        return parse_ipv6(timestamp, payload)
    return None


def parse_ipv4(timestamp, payload):
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
    if payload[9] != 6:
        return None
    src = str(ipaddress.IPv4Address(payload[12:16]))
    dst = str(ipaddress.IPv4Address(payload[16:20]))
    return parse_tcp(timestamp, src, dst, total_length, payload[ihl:total_length])


def parse_ipv6(timestamp, payload):
    if len(payload) < 40:
        return None
    payload_length = struct.unpack("!H", payload[4:6])[0]
    if payload[6] != 6:
        return None
    total_length = 40 + payload_length
    if len(payload) < total_length:
        return None
    src = str(ipaddress.IPv6Address(payload[8:24]))
    dst = str(ipaddress.IPv6Address(payload[24:40]))
    return parse_tcp(timestamp, src, dst, total_length, payload[40:total_length])


def parse_tcp(timestamp, src_host, dst_host, ip_len, payload):
    if len(payload) < 20:
        return None
    src_port, dst_port = struct.unpack("!HH", payload[:4])
    seq = struct.unpack("!I", payload[4:8])[0]
    ack = struct.unpack("!I", payload[8:12])[0]
    data_offset = (payload[12] >> 4) * 4
    if data_offset < 20 or len(payload) < data_offset:
        return None
    data = payload[data_offset:]
    return TcpPacket(
        timestamp=timestamp,
        src=Endpoint(src_host, src_port),
        dst=Endpoint(dst_host, dst_port),
        seq=seq,
        ack=ack,
        ip_len=ip_len,
        tcp_payload_len=len(data),
        flags=payload[13],
        payload=data,
    )


def endpoint_key(endpoint):
    return endpoint.host, endpoint.port


def connection_key(packet):
    lhs = endpoint_key(packet.src)
    rhs = endpoint_key(packet.dst)
    return tuple(sorted((lhs, rhs)))


def group_tcp_connections(packets):
    grouped = defaultdict(list)
    for packet in packets:
        grouped[connection_key(packet)].append(packet)
    return {
        key: sorted(value, key=lambda packet: packet.timestamp)
        for key, value in grouped.items()
    }


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


def infer_client_server(packets, service_port=None):
    for packet in sorted(packets, key=lambda item: item.timestamp):
        if packet.syn and not packet.ack_flag:
            return packet.src, packet.dst
    if service_port is not None:
        for packet in sorted(packets, key=lambda item: item.timestamp):
            if packet.dst.port == service_port:
                return packet.src, packet.dst
            if packet.src.port == service_port:
                return packet.dst, packet.src
    raise PcapError("cannot infer TCP client/server; capture must include SYN or use --port")


def direction_name(src, client, server):
    if src == client:
        return "client_to_server"
    if src == server:
        return "server_to_client"
    return "other"


def reassemble_direction(segments):
    stream, _times = reassemble_direction_with_timestamps(
        [
            TcpPayloadSegment(seq=seq, timestamp=0.0, payload=payload)
            for seq, payload in segments
        ]
    )
    return stream


def reassemble_direction_with_timestamps(segments):
    if not segments:
        return b"", []
    ordered = sorted(segments, key=lambda item: item.seq)
    expected = ordered[0].seq
    out = bytearray()
    byte_times = []
    for segment in ordered:
        seq = segment.seq
        payload = segment.payload
        end = seq + len(payload)
        if end <= expected:
            continue
        if seq < expected:
            payload = payload[expected - seq :]
            seq = expected
        if seq != expected:
            raise PcapError(f"tcp sequence gap: expected {expected}, got {seq}")
        out.extend(payload)
        byte_times.extend([segment.timestamp] * len(payload))
        expected = seq + len(payload)
    return bytes(out), byte_times


def parse_tls_records(stream_bytes):
    records = []
    offset = 0
    while offset < len(stream_bytes):
        if len(stream_bytes) - offset < 5:
            raise PcapError(
                f"trailing {len(stream_bytes) - offset} byte(s) after TLS records"
            )
        content_type = stream_bytes[offset]
        major = stream_bytes[offset + 1]
        minor = stream_bytes[offset + 2]
        length = struct.unpack("!H", stream_bytes[offset + 3 : offset + 5])[0]
        if content_type not in ALLOWED_TLS_CONTENT_TYPES:
            raise PcapError(f"non-TLS content type {content_type} at offset {offset}")
        if major != 3 or minor > 4:
            raise PcapError(
                f"unexpected TLS record version {major}.{minor} at offset {offset}"
            )
        if length > MAX_TLS_RECORD_LENGTH:
            raise PcapError(f"oversized TLS record length {length} at offset {offset}")
        next_offset = offset + 5 + length
        if next_offset > len(stream_bytes):
            raise PcapError(f"truncated TLS record at offset {offset}")
        records.append(
            TlsRecordSummary(
                content_type=content_type,
                version=f"{major}.{minor}",
                length=length,
                wire_size=5 + length,
            )
        )
        offset = next_offset
    return records


def parse_tls_records_from_segments(segments):
    stream_bytes, byte_times = reassemble_direction_with_timestamps(segments)
    records = []
    offset = 0
    while offset < len(stream_bytes):
        if len(stream_bytes) - offset < 5:
            raise PcapError(
                f"trailing {len(stream_bytes) - offset} byte(s) after TLS records"
            )
        content_type = stream_bytes[offset]
        major = stream_bytes[offset + 1]
        minor = stream_bytes[offset + 2]
        length = struct.unpack("!H", stream_bytes[offset + 3 : offset + 5])[0]
        if content_type not in ALLOWED_TLS_CONTENT_TYPES:
            raise PcapError(f"non-TLS content type {content_type} at offset {offset}")
        if major != 3 or minor > 4:
            raise PcapError(
                f"unexpected TLS record version {major}.{minor} at offset {offset}"
            )
        if length > MAX_TLS_RECORD_LENGTH:
            raise PcapError(f"oversized TLS record length {length} at offset {offset}")
        next_offset = offset + 5 + length
        if next_offset > len(stream_bytes):
            raise PcapError(f"truncated TLS record at offset {offset}")
        records.append(
            TlsRecordSummary(
                content_type=content_type,
                version=f"{major}.{minor}",
                length=length,
                wire_size=5 + length,
                timestamp=max(byte_times[offset:next_offset]) if byte_times else None,
            )
        )
        offset = next_offset
    return records
