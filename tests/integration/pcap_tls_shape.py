#!/usr/bin/env python3

import argparse
import ctypes.util
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from fps_https_harness import free_port


SKIP = 77


def skip(reason):
    print(f"SKIP: {reason}", file=sys.stderr)
    return SKIP


def has_passwordless_sudo():
    completed = subprocess.run(
        ["sudo", "-n", "true"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return completed.returncode == 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps-client", required=True)
    parser.add_argument("--fps-server", required=True)
    parser.add_argument("--out-pcap")
    args = parser.parse_args()

    if shutil.which("tcpdump") is None:
        return skip("missing tcpdump executable")
    if ctypes.util.find_library("pcap") is None:
        return skip("missing libpcap runtime")
    if not has_passwordless_sudo():
        return skip("passwordless sudo is required for tcpdump capture")

    repo = Path(__file__).resolve().parents[2]
    capture_tool = repo / "tools" / "capture_tls_wire.sh"
    shape_tool = repo / "tools" / "is_pcap_looks_like_tls.py"
    scenario = repo / "tests" / "integration" / "https_zero_rtt_chain.py"

    origin_port = free_port()
    server_port = free_port()
    client_port = free_port()

    if args.out_pcap:
        pcap_path = Path(args.out_pcap)
        pcap_path.parent.mkdir(parents=True, exist_ok=True)
        summary_path = Path(str(pcap_path) + ".shape.json")
        command_tmpdir = None
    else:
        command_tmpdir = tempfile.TemporaryDirectory()
        pcap_path = Path(command_tmpdir.name) / "fps-zero-rtt-link.pcap"
        summary_path = Path(command_tmpdir.name) / "fps-zero-rtt-link.shape.json"

    try:
        capture_command = [
            str(capture_tool),
            "--port",
            str(server_port),
            "--out",
            str(pcap_path),
            "--",
            sys.executable,
            str(scenario),
            "--fps-client",
            args.fps_client,
            "--fps-server",
            args.fps_server,
            "--origin-port",
            str(origin_port),
            "--server-port",
            str(server_port),
            "--client-port",
            str(client_port),
            "--decoy-allowed-clients",
            "1",
            "--trial-decrypt-limit",
            "1",
        ]
        subprocess.run(capture_command, check=True)

        subprocess.run(
            [
                sys.executable,
                str(shape_tool),
                str(pcap_path),
                "--port",
                str(server_port),
                "--require-bidirectional",
                "--require-application-data",
                "--min-records",
                "4",
                "--summary",
                str(summary_path),
            ],
            check=True,
        )
    finally:
        if command_tmpdir is not None:
            command_tmpdir.cleanup()

    return 0


if __name__ == "__main__":
    sys.exit(main())
