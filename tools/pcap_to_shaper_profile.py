#!/usr/bin/env python3

"""Build an FPS shaper profile from a TLS carrier pcap.

The tool infers TCP client/server roles from the TCP handshake when the capture
contains SYN/SYN-ACK. If the pcap starts after the handshake, --port can be used
as a fallback service-port hint; no client-side/server-side capture argument is
needed.
"""

import argparse
import bisect
import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path

from fps_pcap import (
    TLS_APPLICATION_DATA,
    LibpcapReader,
    PcapError,
    TcpPayloadSegment,
    direction_name,
    group_tcp_connections,
    infer_client_server,
    packet_matches,
    parse_endpoint,
    parse_tls_records_from_segments,
)


@dataclass
class DirectionSamples:
    record_sizes: list[int] = field(default_factory=list)
    inter_record_delay_us: list[int] = field(default_factory=list)


@dataclass
class ProfileSamples:
    client_to_server: DirectionSamples = field(default_factory=DirectionSamples)
    server_to_client: DirectionSamples = field(default_factory=DirectionSamples)
    included_connections: int = 0
    skipped_connections: int = 0
    last_skip_reason: str | None = None


def positive_int(text):
    try:
        value = int(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def nonnegative_float(text):
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a number") from error
    if value < 0.0:
        raise argparse.ArgumentTypeError("must not be negative")
    return value


def probability(text):
    value = nonnegative_float(text)
    if value > 1.0:
        raise argparse.ArgumentTypeError("must be in [0, 1]")
    return value


def empirical_cdf(values, bins):
    if not values:
        raise PcapError("cannot build CDF from an empty sample")
    ordered = sorted(int(value) for value in values)
    count = len(ordered)
    point_count = min(bins, count)
    by_value = {}
    for index in range(1, point_count + 1):
        p = index / float(point_count)
        value_index = max(0, min(count - 1, math.ceil(p * count) - 1))
        value = ordered[value_index]
        by_value[value] = bisect.bisect_right(ordered, value) / float(count)
    out = [[value, probability] for value, probability in sorted(by_value.items())]
    out[-1][1] = 1.0
    return out


def filter_records(records, include_all_tls_records):
    if include_all_tls_records:
        return records
    return [record for record in records if record.content_type == TLS_APPLICATION_DATA]


def add_direction_records(samples, direction, records):
    if not records:
        return
    target = samples.client_to_server if direction == "client_to_server" else samples.server_to_client
    target.record_sizes.extend(record.wire_size for record in records)
    for previous, current in zip(records, records[1:]):
        if previous.timestamp is None or current.timestamp is None:
            continue
        delay_us = int(round((current.timestamp - previous.timestamp) * 1_000_000.0))
        target.inter_record_delay_us.append(max(1, delay_us))


def connection_payload_segments(packets, client, server):
    segments = {
        "client_to_server": [],
        "server_to_client": [],
    }
    for packet in packets:
        if packet.tcp_payload_len == 0:
            continue
        direction = direction_name(packet.src, client, server)
        if direction not in segments:
            continue
        segments[direction].append(
            TcpPayloadSegment(seq=packet.seq, timestamp=packet.timestamp, payload=packet.payload)
        )
    return segments


def collect_samples(args):
    endpoint_a = args.src
    endpoint_b = args.dst
    packets = [
        packet
        for packet in LibpcapReader().read_packets(args.pcap)
        if packet_matches(packet, port=args.port, endpoint_a=endpoint_a, endpoint_b=endpoint_b)
        and (args.start_epoch is None or packet.timestamp >= args.start_epoch)
        and (args.end_epoch is None or packet.timestamp <= args.end_epoch)
    ]
    if not packets:
        raise PcapError("no TCP packets matched the filter")

    samples = ProfileSamples()
    grouped = group_tcp_connections(packets)
    for connection_packets in grouped.values():
        try:
            client, server = infer_client_server(connection_packets, service_port=args.port)
            segments = connection_payload_segments(connection_packets, client, server)
            connection_used = False
            for direction, direction_segments in segments.items():
                if not direction_segments:
                    continue
                records = filter_records(
                    parse_tls_records_from_segments(direction_segments),
                    include_all_tls_records=args.include_all_tls_records,
                )
                if records:
                    add_direction_records(samples, direction, records)
                    connection_used = True
            if connection_used:
                samples.included_connections += 1
            else:
                samples.skipped_connections += 1
                samples.last_skip_reason = "connection had no selected TLS records"
        except PcapError as error:
            samples.skipped_connections += 1
            samples.last_skip_reason = str(error)

    if samples.included_connections == 0:
        reason = f": {samples.last_skip_reason}" if samples.last_skip_reason else ""
        raise PcapError(f"no TLS carrier connections could be profiled{reason}")
    return samples


def validate_sample_counts(samples, min_records_per_direction):
    for name, direction in [
        ("client_to_server", samples.client_to_server),
        ("server_to_client", samples.server_to_client),
    ]:
        if len(direction.record_sizes) < min_records_per_direction:
            raise PcapError(
                f"{name} needs at least {min_records_per_direction} selected TLS records, got {len(direction.record_sizes)}"
            )
        if not direction.inter_record_delay_us:
            raise PcapError(f"{name} needs at least two selected TLS records to estimate delays")


def build_profile(args, samples):
    validate_sample_counts(samples, args.min_records_per_direction)
    return {
        "profile_id": args.profile_id,
        "record_size_cdf_c2s": empirical_cdf(samples.client_to_server.record_sizes, args.bins),
        "record_size_cdf_s2c": empirical_cdf(samples.server_to_client.record_sizes, args.bins),
        "inter_record_delay_us_cdf_c2s": empirical_cdf(samples.client_to_server.inter_record_delay_us, args.bins),
        "inter_record_delay_us_cdf_s2c": empirical_cdf(samples.server_to_client.inter_record_delay_us, args.bins),
        "covert_ratio_max": args.covert_ratio_max,
        "burst_records_max": args.burst_records_max,
        "adaptive": {
            "enabled": args.adaptive_enabled,
            "min_records": args.adaptive_min_records,
            "min_observation_ms": args.adaptive_min_observation_ms,
            "decay": args.adaptive_decay,
            "snapshot_interval_ms": args.snapshot_interval_ms,
        },
    }


def build_summary(samples):
    def direction_summary(direction):
        return {
            "record_count": len(direction.record_sizes),
            "delay_count": len(direction.inter_record_delay_us),
            "record_size_min": min(direction.record_sizes) if direction.record_sizes else None,
            "record_size_max": max(direction.record_sizes) if direction.record_sizes else None,
            "delay_us_min": min(direction.inter_record_delay_us) if direction.inter_record_delay_us else None,
            "delay_us_max": max(direction.inter_record_delay_us) if direction.inter_record_delay_us else None,
        }

    return {
        "included_connections": samples.included_connections,
        "skipped_connections": samples.skipped_connections,
        "client_to_server": direction_summary(samples.client_to_server),
        "server_to_client": direction_summary(samples.server_to_client),
    }


def write_output(path, text, force):
    output_path = Path(path)
    if output_path.exists() and not force:
        raise PcapError(f"{output_path} already exists; use --force to overwrite")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(text, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(
        description="Build a static FPS shaper CDF profile from a TLS carrier pcap."
    )
    parser.add_argument("pcap")
    parser.add_argument("--port", type=positive_int, help="match TCP packets with this source or destination port")
    parser.add_argument("--src", type=parse_endpoint, help="source endpoint for exact src/dst filter")
    parser.add_argument("--dst", type=parse_endpoint, help="destination endpoint for exact src/dst filter")
    parser.add_argument("--start-epoch", type=float, help="ignore packets before this UNIX timestamp")
    parser.add_argument("--end-epoch", type=float, help="ignore packets after this UNIX timestamp")
    parser.add_argument("--profile-id", default="pcap-carrier-profile-v1")
    parser.add_argument("--bins", type=positive_int, default=50, help="maximum CDF points per distribution")
    parser.add_argument("--min-records-per-direction", type=positive_int, default=2)
    parser.add_argument("--covert-ratio-max", type=probability, default=0.25)
    parser.add_argument("--burst-records-max", type=positive_int, default=1)
    parser.add_argument("--adaptive-enabled", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--adaptive-min-records", type=positive_int, default=16)
    parser.add_argument("--adaptive-min-observation-ms", type=positive_int, default=2000)
    parser.add_argument("--adaptive-decay", type=probability, default=0.98)
    parser.add_argument("--snapshot-interval-ms", type=positive_int, default=30000)
    parser.add_argument(
        "--include-all-tls-records",
        action="store_true",
        help="include handshake/alert/change-cipher-spec records; default uses Application Data only",
    )
    parser.add_argument("--output", help="write shaper profile JSON to this path")
    parser.add_argument("--summary", help="write non-secret sample summary JSON to this path")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    if (args.src is None) != (args.dst is None):
        parser.error("--src and --dst must be provided together")
    if args.port is not None and args.port > 65535:
        parser.error("--port out of range")
    if args.start_epoch is not None and args.end_epoch is not None and args.start_epoch > args.end_epoch:
        parser.error("--start-epoch must be <= --end-epoch")
    if not args.profile_id:
        parser.error("--profile-id must not be empty")
    if args.adaptive_decay <= 0.0:
        parser.error("--adaptive-decay must be in (0, 1]")

    try:
        samples = collect_samples(args)
        profile = build_profile(args, samples)
        profile_text = json.dumps(profile, indent=2, sort_keys=True) + "\n"
        if args.output:
            write_output(args.output, profile_text, args.force)
        else:
            print(profile_text, end="")
        if args.summary:
            summary_text = json.dumps(build_summary(samples), indent=2, sort_keys=True) + "\n"
            write_output(args.summary, summary_text, args.force)
    except PcapError as error:
        print(f"failed to build shaper profile: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
