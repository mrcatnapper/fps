#!/usr/bin/env python3

import argparse
import csv
import json
import math
import sys
from pathlib import Path

from fps_pcap import LibpcapReader, PcapError, packet_matches, parse_endpoint


class PcapFlowError(PcapError):
    pass


def quantile(sorted_values, probability):
    if not sorted_values:
        return None
    if len(sorted_values) == 1:
        return sorted_values[0]
    position = (len(sorted_values) - 1) * probability
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return sorted_values[lower]
    weight = position - lower
    return sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight


def quantiles(values):
    ordered = sorted(values)
    return {
        "count": len(ordered),
        "min": ordered[0] if ordered else None,
        "p01": quantile(ordered, 0.01),
        "p05": quantile(ordered, 0.05),
        "p10": quantile(ordered, 0.10),
        "p25": quantile(ordered, 0.25),
        "p50": quantile(ordered, 0.50),
        "p75": quantile(ordered, 0.75),
        "p90": quantile(ordered, 0.90),
        "p95": quantile(ordered, 0.95),
        "p99": quantile(ordered, 0.99),
        "max": ordered[-1] if ordered else None,
    }


def summarize_packets(packets):
    if not packets:
        return {
            "packet_count": 0,
            "tcp_payload_packet_count": 0,
            "ip_len": quantiles([]),
            "tcp_payload_len": quantiles([]),
            "inter_packet_ms": quantiles([]),
        }
    times = [packet.timestamp for packet in packets]
    inter_ms = [
        (current.timestamp - previous.timestamp) * 1000.0
        for previous, current in zip(packets, packets[1:])
        if current.timestamp >= previous.timestamp
    ]
    duration = max(times) - min(times) if len(times) > 1 else 0.0
    bytes_total = sum(packet.ip_len for packet in packets)
    payload_bytes_total = sum(packet.tcp_payload_len for packet in packets)
    return {
        "packet_count": len(packets),
        "tcp_payload_packet_count": sum(1 for packet in packets if packet.tcp_payload_len > 0),
        "duration_sec": duration,
        "ip_bytes_total": bytes_total,
        "tcp_payload_bytes_total": payload_bytes_total,
        "ip_mbit_per_sec": (bytes_total * 8.0 / duration / 1_000_000.0) if duration > 0 else 0.0,
        "tcp_payload_mbit_per_sec": (payload_bytes_total * 8.0 / duration / 1_000_000.0) if duration > 0 else 0.0,
        "ip_len": quantiles([packet.ip_len for packet in packets]),
        "tcp_payload_len": quantiles([packet.tcp_payload_len for packet in packets]),
        "inter_packet_ms": quantiles(inter_ms),
    }


def direction_key(packet, server_port):
    if server_port is None:
        return f"{packet.src.host}:{packet.src.port}->{packet.dst.host}:{packet.dst.port}"
    if packet.dst.port == server_port:
        return "client_to_server"
    if packet.src.port == server_port:
        return "server_to_client"
    return "other"


def split_packets(packets, split_epoch):
    before = [packet for packet in packets if packet.timestamp < split_epoch]
    after = [packet for packet in packets if packet.timestamp >= split_epoch]
    return before, after


def write_csv(path, packets, start_time, split_epoch, server_port):
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "timestamp_epoch",
                "relative_ms",
                "phase",
                "direction",
                "src",
                "dst",
                "ip_len",
                "tcp_payload_len",
                "flags",
                "inter_packet_ms",
            ]
        )
        previous = None
        for packet in packets:
            inter_ms = "" if previous is None else f"{(packet.timestamp - previous.timestamp) * 1000.0:.6f}"
            previous = packet
            writer.writerow(
                [
                    f"{packet.timestamp:.6f}",
                    f"{(packet.timestamp - start_time) * 1000.0:.3f}",
                    "before" if packet.timestamp < split_epoch else "after",
                    direction_key(packet, server_port),
                    f"{packet.src.host}:{packet.src.port}",
                    f"{packet.dst.host}:{packet.dst.port}",
                    packet.ip_len,
                    packet.tcp_payload_len,
                    f"0x{packet.flags:02x}",
                    inter_ms,
                ]
            )


def color_for_intensity(value):
    value = max(0.0, min(1.0, value))
    red = int(32 + value * 210)
    green = int(80 + value * 110)
    blue = int(160 - value * 120)
    return f"rgb({red},{green},{blue})"


def svg_escape(text):
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def make_bins(values, count, lower=None, upper=None):
    if not values:
        return []
    lo = min(values) if lower is None else lower
    hi = max(values) if upper is None else upper
    if hi <= lo:
        hi = lo + 1.0
    step = (hi - lo) / float(count)
    return [lo + step * index for index in range(count + 1)]


def bin_index(value, bins):
    if value < bins[0] or value > bins[-1]:
        return None
    if value == bins[-1]:
        return len(bins) - 2
    width = bins[1] - bins[0]
    if width <= 0:
        return None
    index = int((value - bins[0]) / width)
    if index < 0 or index >= len(bins) - 1:
        return None
    return index


def write_svg(path, packets, start_time, end_time, split_epoch, server_port):
    width = 1200
    panel_height = 280
    margin_left = 90
    margin_right = 30
    margin_top = 40
    gap = 70
    height = margin_top + panel_height * 3 + gap * 2 + 60
    plot_width = width - margin_left - margin_right
    duration = max(end_time - start_time, 0.001)
    max_ip = max([packet.ip_len for packet in packets] or [1])
    inter_values = [
        (current.timestamp - previous.timestamp) * 1000.0
        for previous, current in zip(packets, packets[1:])
        if current.timestamp >= previous.timestamp
    ]
    inter_cap = quantile(sorted(inter_values), 0.99) if inter_values else 1.0
    inter_cap = max(float(inter_cap or 1.0), 1.0)

    def x_at(timestamp):
        return margin_left + (timestamp - start_time) / duration * plot_width

    def y_at_size(value, top):
        return top + panel_height - (float(value) / float(max_ip)) * panel_height

    def y_at_inter(value, top):
        capped = min(float(value), inter_cap)
        return top + panel_height - (capped / inter_cap) * panel_height

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<style>text{font-family:monospace;font-size:13px} .axis{stroke:#333;stroke-width:1} .grid{stroke:#ddd;stroke-width:1}</style>',
        f'<text x="{margin_left}" y="24">FPS TCP flow pcap analysis; split marks first observed upgrade/authentication time</text>',
    ]

    split_x = x_at(split_epoch)
    panels = [
        ("IP packet size over time", margin_top, "ip_len"),
        ("Inter-packet time over time, capped at p99", margin_top + panel_height + gap, "inter"),
        ("Packet-size heatmap by time", margin_top + (panel_height + gap) * 2, "heat"),
    ]
    for title, top, _kind in panels:
        lines.append(f'<text x="12" y="{top + 16}">{svg_escape(title)}</text>')
        lines.append(f'<line class="axis" x1="{margin_left}" y1="{top}" x2="{margin_left}" y2="{top + panel_height}"/>')
        lines.append(f'<line class="axis" x1="{margin_left}" y1="{top + panel_height}" x2="{width - margin_right}" y2="{top + panel_height}"/>')
        lines.append(f'<line x1="{split_x:.2f}" y1="{top}" x2="{split_x:.2f}" y2="{top + panel_height}" stroke="#b00020" stroke-width="2"/>')
        lines.append(f'<text x="{split_x + 4:.2f}" y="{top + 14}" fill="#b00020">upgrade</text>')

    for packet in packets:
        x = x_at(packet.timestamp)
        direction = direction_key(packet, server_port)
        color = "#0057b8" if direction == "client_to_server" else "#d04a02"
        y = y_at_size(packet.ip_len, panels[0][1])
        radius = 1.8 if packet.tcp_payload_len > 0 else 1.1
        lines.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{radius}" fill="{color}" opacity="0.55"/>')

    for previous, current in zip(packets, packets[1:]):
        if current.timestamp < previous.timestamp:
            continue
        value = (current.timestamp - previous.timestamp) * 1000.0
        x = x_at(current.timestamp)
        y = y_at_inter(value, panels[1][1])
        direction = direction_key(current, server_port)
        color = "#0057b8" if direction == "client_to_server" else "#d04a02"
        lines.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="1.5" fill="{color}" opacity="0.55"/>')

    time_bins = make_bins([packet.timestamp for packet in packets], 80, start_time, end_time)
    size_bins = make_bins([packet.ip_len for packet in packets], 40, 0, max_ip)
    cells = {}
    for packet in packets:
        tx = bin_index(packet.timestamp, time_bins)
        sy = bin_index(packet.ip_len, size_bins)
        if tx is None or sy is None:
            continue
        cells[(tx, sy)] = cells.get((tx, sy), 0) + 1
    max_count = max(cells.values() or [1])
    heat_top = panels[2][1]
    cell_w = plot_width / max(1, len(time_bins) - 1)
    cell_h = panel_height / max(1, len(size_bins) - 1)
    for (tx, sy), count in cells.items():
        x = margin_left + tx * cell_w
        y = heat_top + panel_height - (sy + 1) * cell_h
        color = color_for_intensity(math.log1p(count) / math.log1p(max_count))
        lines.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{cell_w + 0.5:.2f}" height="{cell_h + 0.5:.2f}" fill="{color}" opacity="0.85"/>')

    lines.extend(
        [
            f'<text x="{margin_left}" y="{height - 30}" fill="#0057b8">blue: client_to_server</text>',
            f'<text x="{margin_left + 260}" y="{height - 30}" fill="#d04a02">orange: server_to_client</text>',
            f'<text x="{margin_left + 560}" y="{height - 30}">IP size max={max_ip} bytes; inter-packet p99 cap={inter_cap:.3f} ms</text>',
            "</svg>",
        ]
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_summary(packets, split_epoch, server_port):
    before, after = split_packets(packets, split_epoch)
    summary = {
        "capture": {
            "start_epoch": packets[0].timestamp if packets else None,
            "end_epoch": packets[-1].timestamp if packets else None,
            "duration_sec": (packets[-1].timestamp - packets[0].timestamp) if len(packets) > 1 else 0.0,
            "split_epoch": split_epoch,
            "split_after_capture_start_sec": (split_epoch - packets[0].timestamp) if packets else None,
        },
        "all": summarize_packets(packets),
        "before_upgrade": summarize_packets(before),
        "after_upgrade": summarize_packets(after),
        "directions": {},
    }
    for phase_name, phase_packets in [("before_upgrade", before), ("after_upgrade", after)]:
        directions = {}
        for packet in phase_packets:
            key = direction_key(packet, server_port)
            directions.setdefault(key, []).append(packet)
        summary["directions"][phase_name] = {
            key: summarize_packets(value) for key, value in sorted(directions.items())
        }
    return summary


def main():
    parser = argparse.ArgumentParser(
        description="Analyze TCP packet sizes and inter-packet timing around an FPS upgrade split."
    )
    parser.add_argument("pcap")
    parser.add_argument("--port", type=int, required=True, help="match TCP packets with this source or destination port")
    parser.add_argument("--src", type=parse_endpoint, help="source endpoint for exact src/dst filter")
    parser.add_argument("--dst", type=parse_endpoint, help="destination endpoint for exact src/dst filter")
    parser.add_argument("--split-time-epoch", type=float, required=True)
    parser.add_argument("--summary-json", required=True)
    parser.add_argument("--packets-csv", required=True)
    parser.add_argument("--svg", required=True)
    args = parser.parse_args()

    if (args.src is None) != (args.dst is None):
        parser.error("--src and --dst must be provided together")
    if args.port <= 0 or args.port > 65535:
        parser.error("--port out of range")

    endpoint_a = args.src
    endpoint_b = args.dst
    packets = [
        packet
        for packet in LibpcapReader().read_packets(args.pcap)
        if packet_matches(packet, port=args.port, endpoint_a=endpoint_a, endpoint_b=endpoint_b)
    ]
    packets.sort(key=lambda packet: packet.timestamp)
    if not packets:
        raise PcapFlowError("no TCP packets matched the filter")

    summary = build_summary(packets, args.split_time_epoch, args.port)
    summary_path = Path(args.summary_json)
    csv_path = Path(args.packets_csv)
    svg_path = Path(args.svg)
    for path in (summary_path, csv_path, svg_path):
        path.parent.mkdir(parents=True, exist_ok=True)

    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_csv(csv_path, packets, packets[0].timestamp, args.split_time_epoch, args.port)
    write_svg(svg_path, packets, packets[0].timestamp, packets[-1].timestamp, args.split_time_epoch, args.port)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PcapError as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
