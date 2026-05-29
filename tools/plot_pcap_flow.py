#!/usr/bin/env python3
"""Render readable FPS pcap flow plots from analyzer CSV/JSON artifacts.

This is an optional traffic-analysis helper, not a project runtime dependency.
It requires a developer venv or globally installed Python packages:

    python3 -m venv .venv
    .venv/bin/python -m pip install matplotlib pandas numpy
    .venv/bin/python tools/plot_pcap_flow.py ...
"""

import argparse
import json
import sys
from pathlib import Path


def import_plotting_deps():
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
        import pandas as pd
    except ModuleNotFoundError as error:
        raise RuntimeError(
            "missing optional plotting dependency. Install a local venv with: "
            "python3 -m venv .venv && "
            ".venv/bin/python -m pip install matplotlib pandas numpy"
        ) from error
    return plt, np, pd


def load_inputs(packets_csv: Path, summary_json: Path):
    plt, np, pd = import_plotting_deps()
    packets = pd.read_csv(packets_csv)
    summary = json.loads(summary_json.read_text(encoding="utf-8"))
    if packets.empty:
        raise RuntimeError("packet CSV is empty")
    required = {
        "relative_ms",
        "phase",
        "direction",
        "ip_len",
        "tcp_payload_len",
        "inter_packet_ms",
    }
    missing = required - set(packets.columns)
    if missing:
        raise RuntimeError(f"packet CSV missing required columns: {sorted(missing)}")
    packets["relative_sec"] = packets["relative_ms"].astype(float) / 1000.0
    packets["ip_len"] = packets["ip_len"].astype(float)
    packets["tcp_payload_len"] = packets["tcp_payload_len"].astype(float)
    packets["inter_packet_ms"] = pd.to_numeric(packets["inter_packet_ms"], errors="coerce")
    return plt, np, pd, packets, summary


def sample_for_scatter(packets, sample_size: int, random_state: int):
    if sample_size <= 0 or len(packets) <= sample_size:
        return packets
    return packets.sample(n=sample_size, random_state=random_state).sort_values("relative_sec")


def phase_color(phase):
    return "#606060" if phase == "before" else "#1f77b4"


def direction_color(direction):
    if direction == "client_to_server":
        return "#0057b8"
    if direction == "server_to_client":
        return "#d04a02"
    return "#666666"


def add_split_line(axis, summary):
    split = summary.get("capture", {}).get("split_after_capture_start_sec")
    if split is None:
        return
    axis.axvline(float(split), color="#b00020", linewidth=1.4, linestyle="--", alpha=0.85)
    axis.text(
        float(split),
        0.98,
        " upgrade",
        color="#b00020",
        transform=axis.get_xaxis_transform(),
        va="top",
        ha="left",
        fontsize=9,
    )


def quantile_text(summary, phase):
    data = summary.get(phase, {})
    ip = data.get("ip_len", {})
    inter = data.get("inter_packet_ms", {})
    return (
        f"{phase.replace('_', ' ')}\n"
        f"packets: {data.get('packet_count', 0):,}\n"
        f"IP Mbps: {data.get('ip_mbit_per_sec', 0.0):.2f}\n"
        f"size p50/p95: {ip.get('p50', 0):.0f}/{ip.get('p95', 0):.0f} B\n"
        f"ipt p50/p95: {inter.get('p50', 0):.3g}/{inter.get('p95', 0):.3g} ms"
    )


def render_overview(plt, np, packets, summary, out_prefix: Path, sample_size: int):
    sample = sample_for_scatter(packets, sample_size, 7)
    fig = plt.figure(figsize=(16, 11), constrained_layout=True)
    grid = fig.add_gridspec(3, 2, height_ratios=[1.1, 1.1, 1.0])

    axis_size = fig.add_subplot(grid[0, :])
    axis_inter = fig.add_subplot(grid[1, :], sharex=axis_size)
    axis_hist = fig.add_subplot(grid[2, 0])
    axis_heat = fig.add_subplot(grid[2, 1])

    for direction, group in sample.groupby("direction", sort=False):
        axis_size.scatter(
            group["relative_sec"],
            group["ip_len"],
            s=5,
            alpha=0.38,
            linewidths=0,
            color=direction_color(direction),
            label=direction,
        )
    axis_size.set_title("Visible FPS TCP-link packet size over capture time")
    axis_size.set_ylabel("IP packet length, bytes")
    axis_size.grid(True, linewidth=0.4, alpha=0.35)
    axis_size.legend(loc="upper right")
    add_split_line(axis_size, summary)

    inter_sample = sample.dropna(subset=["inter_packet_ms"])
    for direction, group in inter_sample.groupby("direction", sort=False):
        axis_inter.scatter(
            group["relative_sec"],
            group["inter_packet_ms"].clip(lower=0.001),
            s=5,
            alpha=0.35,
            linewidths=0,
            color=direction_color(direction),
            label=direction,
        )
    axis_inter.set_yscale("log")
    axis_inter.set_title("Inter-packet time over capture time, log scale")
    axis_inter.set_ylabel("Inter-packet time, ms")
    axis_inter.set_xlabel("Capture time, seconds")
    axis_inter.grid(True, which="both", linewidth=0.4, alpha=0.35)
    add_split_line(axis_inter, summary)

    bins = np.linspace(0, max(1600.0, packets["ip_len"].quantile(0.995)), 80)
    for phase, group in packets.groupby("phase", sort=False):
        axis_hist.hist(
            group["ip_len"],
            bins=bins,
            alpha=0.55,
            density=True,
            color=phase_color(phase),
            label=phase,
        )
    axis_hist.set_title("Packet-size distribution, before vs after")
    axis_hist.set_xlabel("IP packet length, bytes")
    axis_hist.set_ylabel("Density")
    axis_hist.grid(True, linewidth=0.4, alpha=0.35)
    axis_hist.legend()

    heat = packets[packets["phase"] == "after"]
    if heat.empty:
        heat = packets
    hexbin = axis_heat.hexbin(
        heat["relative_sec"],
        heat["ip_len"],
        gridsize=(80, 45),
        mincnt=1,
        bins="log",
        cmap="viridis",
    )
    axis_heat.set_title("Packet-size heatmap after upgrade")
    axis_heat.set_xlabel("Capture time, seconds")
    axis_heat.set_ylabel("IP packet length, bytes")
    axis_heat.grid(True, linewidth=0.25, alpha=0.20)
    fig.colorbar(hexbin, ax=axis_heat, label="log10(packet count)")

    fig.text(
        0.01,
        0.01,
        quantile_text(summary, "before_upgrade") + "\n\n" + quantile_text(summary, "after_upgrade"),
        ha="left",
        va="bottom",
        fontsize=9,
        family="monospace",
        bbox={"boxstyle": "round,pad=0.45", "facecolor": "white", "edgecolor": "#cccccc", "alpha": 0.92},
    )

    png_path = out_prefix.with_suffix(".overview.png")
    svg_path = out_prefix.with_suffix(".overview.svg")
    fig.savefig(png_path, dpi=160)
    fig.savefig(svg_path)
    plt.close(fig)
    return [png_path, svg_path]


def render_direction_quantiles(plt, np, packets, summary, out_prefix: Path):
    fig, axes = plt.subplots(1, 2, figsize=(14, 5), constrained_layout=True)
    directions = ["client_to_server", "server_to_client"]
    phases = ["before", "after"]
    width = 0.35

    for axis, metric, title, ylabel in [
        (axes[0], "ip_len", "Packet-size quantiles by direction", "IP packet length, bytes"),
        (axes[1], "inter_packet_ms", "Inter-packet p50/p95 by direction", "Inter-packet time, ms"),
    ]:
        x = np.arange(len(directions))
        for index, phase in enumerate(phases):
            values = []
            errors = []
            for direction in directions:
                group = packets[(packets["phase"] == phase) & (packets["direction"] == direction)]
                series = group[metric].dropna().astype(float)
                if series.empty:
                    values.append(0.0)
                    errors.append(0.0)
                    continue
                p50 = float(series.quantile(0.50))
                p95 = float(series.quantile(0.95))
                values.append(p50)
                errors.append(max(0.0, p95 - p50))
            offset = (index - 0.5) * width
            axis.bar(
                x + offset,
                values,
                width,
                yerr=[np.zeros(len(errors)), np.array(errors)],
                capsize=4,
                label=phase,
                alpha=0.78,
            )
        axis.set_title(title)
        axis.set_ylabel(ylabel)
        axis.set_xticks(x)
        axis.set_xticklabels(directions, rotation=10)
        axis.grid(True, axis="y", linewidth=0.4, alpha=0.35)
        axis.legend()
    axes[1].set_yscale("log")

    png_path = out_prefix.with_suffix(".quantiles.png")
    svg_path = out_prefix.with_suffix(".quantiles.svg")
    fig.savefig(png_path, dpi=160)
    fig.savefig(svg_path)
    plt.close(fig)
    return [png_path, svg_path]


def main():
    parser = argparse.ArgumentParser(
        description="Render publication-quality FPS pcap flow plots from flow-packets.csv."
    )
    parser.add_argument("--packets-csv", required=True)
    parser.add_argument("--summary-json", required=True)
    parser.add_argument("--out-prefix", required=True)
    parser.add_argument(
        "--sample-size",
        type=int,
        default=25000,
        help="maximum scatter points in overview plot; 0 means use all packets",
    )
    args = parser.parse_args()

    packets_csv = Path(args.packets_csv)
    summary_json = Path(args.summary_json)
    out_prefix = Path(args.out_prefix)
    out_prefix.parent.mkdir(parents=True, exist_ok=True)

    plt, np, _pd, packets, summary = load_inputs(packets_csv, summary_json)
    outputs = []
    outputs.extend(render_overview(plt, np, packets, summary, out_prefix, args.sample_size))
    outputs.extend(render_direction_quantiles(plt, np, packets, summary, out_prefix))
    for path in outputs:
        print(path)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
