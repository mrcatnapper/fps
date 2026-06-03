#!/usr/bin/env python3

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

from fps_pcap import (
    TLS_APPLICATION_DATA,
    LibpcapReader,
    PcapError,
    packet_matches,
    parse_endpoint,
    parse_tls_records,
    reassemble_direction,
)


class PcapShapeError(PcapError):
    pass


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
        if packet.tcp_payload_len > 0
        and packet_matches(packet, port=port, endpoint_a=endpoint_a, endpoint_b=endpoint_b)
    ]
    if not packets:
        raise PcapShapeError("no TCP payload packets matched the filter")

    segments = defaultdict(list)
    for packet in packets:
        key = (packet.src, packet.dst)
        segments[key].append((packet.seq, packet.payload))

    streams = []
    for (src, dst), direction_segments in sorted(
        segments.items(),
        key=lambda item: (
            item[0][0].host,
            item[0][0].port,
            item[0][1].host,
            item[0][1].port,
        ),
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
    except PcapError as error:
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
