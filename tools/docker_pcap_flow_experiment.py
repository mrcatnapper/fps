#!/usr/bin/env python3

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
from pathlib import Path

from docker_tun_iperf_sim import (
    CLIENT_UUID,
    compose,
    compose_exec,
    docker,
    docker_base,
    generate_server_keypair,
    repo_root,
    wait_for_client_lease,
    wait_for_log,
    wait_for_service_set,
    wait_for_tun,
    write_config,
)


REQUIRED_SERVICES_BASE = [
    "fps-carrier-origin",
    "fps-server",
    "fps-client",
]
REQUIRED_SERVICES_ALL = [
    *REQUIRED_SERVICES_BASE,
    "fps-carrier-client",
]


def run(args, *, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT):
    completed = subprocess.run(args, text=True, stdout=stdout, stderr=stderr)
    if check and completed.returncode != 0:
        command = " ".join(str(arg) for arg in args)
        output = completed.stdout if completed.stdout is not None else ""
        raise RuntimeError(f"command failed ({completed.returncode}): {command}\n{output}")
    return completed


def write_experiment_compose(
    path: Path,
    image: str,
    log_level: str,
    mtu: int,
    carrier_bps: int,
    carrier_frame_rate: float,
):
    path.write_text(
        textwrap.dedent(
            f"""
            services:
              fps-carrier-origin:
                image: {image}
                command:
                  - fps_carrier
                  - origin
                  - --listen
                  - 0.0.0.0:9443
                  - --cert
                  - /tmp/fps-carrier/cert.pem
                  - --key
                  - /tmp/fps-carrier/key.pem
                  - --generate-self-signed
                  - --path
                  - /fps-carrier
                  - --max-frame-size
                  - "65536"
                restart: unless-stopped
                security_opt:
                  - no-new-privileges:true

              fps-server:
                image: {image}
                depends_on:
                  - fps-carrier-origin
                restart: unless-stopped
                environment:
                  FPS_ROLE: server
                  FPS_CONFIG: /etc/fps/server.json
                  FPS_LOG_LEVEL: {log_level}
                  FPS_CONFIGURE_TUN: "1"
                  FPS_TUN_NAME: fpss0
                  FPS_TUN_ADDRESS: 10.88.0.1/30
                  FPS_TUN_MTU: "{mtu}"
                volumes:
                  - ./config/server.json:/etc/fps/server.json:ro
                devices:
                  - /dev/net/tun:/dev/net/tun
                cap_add:
                  - NET_ADMIN
                  - NET_BIND_SERVICE
                security_opt:
                  - no-new-privileges:true

              fps-client:
                image: {image}
                depends_on:
                  - fps-server
                restart: unless-stopped
                environment:
                  FPS_ROLE: client
                  FPS_CONFIG: /etc/fps/client.json
                  FPS_LOG_LEVEL: {log_level}
                  FPS_CONFIGURE_TUN: "1"
                  FPS_TUN_NAME: fpsc0
                  FPS_TUN_MTU: "{mtu}"
                volumes:
                  - ./config/client.json:/etc/fps/client.json:ro
                devices:
                  - /dev/net/tun:/dev/net/tun
                cap_add:
                  - NET_ADMIN
                  - NET_BIND_SERVICE
                security_opt:
                  - no-new-privileges:true

              fps-carrier-client:
                image: {image}
                depends_on:
                  - fps-client
                command:
                  - fps_carrier
                  - client
                  - --connect
                  - fps-client:7443
                  - --path
                  - /fps-carrier
                  - --client-bps
                  - "{carrier_bps}"
                  - --frame-rate
                  - "{carrier_frame_rate}"
                  - --min-frame-size
                  - "256"
                  - --max-frame-size
                  - "16384"
                  - --jitter
                  - "0.15"
                  - --seed
                  - "424242"
                restart: unless-stopped
                security_opt:
                  - no-new-privileges:true
            """
        ).strip()
        + "\n",
        encoding="utf-8",
    )


def edit_zero_rtt_trial_records(path: Path, count: int):
    data = json.loads(path.read_text(encoding="utf-8"))
    data["security"]["zero_rtt"]["min_records_before_trial"] = count
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def tcpdump_command(interface: str, pcap_path: Path, port: int):
    command = [
        "tcpdump",
        "--immediate-mode",
        "-i",
        interface,
        "-s",
        "0",
        "-U",
        "-w",
        str(pcap_path),
        f"tcp port {port}",
    ]
    if shutil.which("aa-exec") is not None:
        command = ["aa-exec", "-p", "unconfined", "--", *command]
    return ["sudo", "-n", *command]


def start_capture(interface: str, pcap_path: Path, port: int):
    pcap_path.parent.mkdir(parents=True, exist_ok=True)
    if pcap_path.exists():
        pcap_path.unlink()
    process = subprocess.Popen(
        tcpdump_command(interface, pcap_path, port),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(0.5)
    if process.poll() is not None:
        raise RuntimeError(f"tcpdump exited early with status {process.returncode}")
    return process


def resolve_capture_interface(base, project: str, requested: str) -> str:
    if requested != "auto":
        return requested
    network_name = f"{project}_default"
    result = docker(base, ["network", "inspect", network_name, "--format", "{{json .}}"])
    data = json.loads(result.stdout)
    bridge_name = data.get("Options", {}).get("com.docker.network.bridge.name")
    if not bridge_name:
        bridge_name = f"br-{data['Id'][:12]}"
    if Path("/sys/class/net", bridge_name).exists():
        return bridge_name
    return "any"


def stop_capture(process):
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5.0)


def analyze(repo: Path, pcap_path: Path, split_epoch: float, out_dir: Path, port: int):
    summary = out_dir / "flow-summary.json"
    packets = out_dir / "flow-packets.csv"
    svg = out_dir / "flow-plot.svg"
    run(
        [
            sys.executable,
            str(repo / "tools" / "analyze_pcap_tcp_flow.py"),
            str(pcap_path),
            "--port",
            str(port),
            "--split-time-epoch",
            f"{split_epoch:.6f}",
            "--summary-json",
            str(summary),
            "--packets-csv",
            str(packets),
            "--svg",
            str(svg),
        ]
    )
    return json.loads(summary.read_text(encoding="utf-8"))


def parse_iperf_udp_summaries(output: str) -> dict:
    data = json.loads(output)
    end = data.get("end", {})
    summaries = {}
    for key, value in sorted(end.items()):
        if not isinstance(value, dict) or "bits_per_second" not in value:
            continue
        packets = value.get("packets", 0) or 0
        lost_packets = value.get("lost_packets", 0) or 0
        lost_percent = value.get("lost_percent")
        if lost_percent is None and packets:
            lost_percent = (lost_packets / packets) * 100.0
        summaries[key] = {
            "bits_per_second": value.get("bits_per_second", 0.0),
            "mbits_per_second": value.get("bits_per_second", 0.0) / 1_000_000.0,
            "jitter_ms": value.get("jitter_ms", 0.0),
            "lost_packets": lost_packets,
            "packets": packets,
            "lost_percent": lost_percent if lost_percent is not None else 0.0,
            "seconds": value.get("seconds", 0.0),
            "bytes": value.get("bytes", 0),
        }
    if not summaries:
        raise RuntimeError(f"cannot find UDP summary in iperf3 JSON:\n{output}")
    return summaries


def wait_for_running_services(base, compose_file, project, services, timeout):
    deadline = time.monotonic() + timeout
    last = ""
    expected = set(services)
    while time.monotonic() < deadline:
        result = compose(base, compose_file, project, ["ps", "--services", "--status", "running"])
        running = set(result.stdout.split())
        last = result.stdout
        if expected.issubset(running):
            return
        time.sleep(0.5)
    raise RuntimeError(f"services did not become running: expected={sorted(expected)} running={last}")


def ignore_private_artifacts(_directory, names):
    return {name for name in names if name.endswith(".key")}


def main():
    parser = argparse.ArgumentParser(
        description="Capture and analyze FPS TCP-link packet shape before/after Zero-RTT upgrade."
    )
    parser.add_argument("--image", default=os.environ.get("FPS_DOCKER_IMAGE", "fps:local"))
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--sudo", action="store_true", help="Use sudo -n docker.")
    parser.add_argument("--project", default=f"fps-pcap-flow-{os.getpid()}")
    parser.add_argument("--out-dir", default=None)
    parser.add_argument("--duration", type=int, default=30)
    parser.add_argument("--bandwidth", default="300K")
    parser.add_argument("--iperf-bidir", action="store_true", help="Run bidirectional UDP iperf3.")
    parser.add_argument("--length", type=int, default=1000)
    parser.add_argument("--mtu", type=int, default=1280)
    parser.add_argument("--carrier-bps", type=int, default=300000)
    parser.add_argument("--carrier-frame-rate", type=float, default=30.0)
    parser.add_argument("--pre-upgrade-records", type=int, default=60)
    parser.add_argument(
        "--capture-interface",
        default="auto",
        help="pcap interface; 'auto' resolves the Docker bridge, fallback is 'any'",
    )
    parser.add_argument("--capture-port", type=int, default=8443)
    parser.add_argument("--log-level", default="info")
    parser.add_argument("--startup-timeout", type=float, default=45.0)
    parser.add_argument("--keep-artifacts", action="store_true")
    args = parser.parse_args()

    if args.duration <= 0:
        raise ValueError("--duration must be positive")
    if args.length <= 0:
        raise ValueError("--length must be positive")
    if args.carrier_bps <= 0:
        raise ValueError("--carrier-bps must be positive")
    if args.carrier_frame_rate <= 0:
        raise ValueError("--carrier-frame-rate must be positive")
    if args.pre_upgrade_records <= 0:
        raise ValueError("--pre-upgrade-records must be positive")

    base = docker_base(args.sudo)
    root = repo_root()
    if args.build:
        docker(base, ["build", "-t", args.image, str(root)])

    output_root = Path(args.out_dir) if args.out_dir else root / "captures" / args.project
    if output_root.exists():
        shutil.rmtree(output_root)
    output_root.mkdir(parents=True)

    with tempfile.TemporaryDirectory(prefix="fps-docker-pcap-flow-") as tmp:
        work = Path(tmp)
        config_dir = work / "config"
        config_dir.mkdir()
        server_keys = generate_server_keypair(base, args.image)
        write_config(
            config_dir / "server.json",
            "server",
            "fps-carrier-origin:9443",
            "fpss0",
            args.mtu,
            "docker-pcap-flow-v4",
            CLIENT_UUID,
            server_keys["server_private_key_base64"],
            server_keys["server_public_key_base64"],
        )
        write_config(
            config_dir / "client.json",
            "client",
            "fps-server:8443",
            "fpsc0",
            args.mtu,
            "docker-pcap-flow-v4",
            CLIENT_UUID,
            server_keys["server_private_key_base64"],
            server_keys["server_public_key_base64"],
        )
        edit_zero_rtt_trial_records(config_dir / "client.json", args.pre_upgrade_records)
        compose_file = work / "compose.yml"
        write_experiment_compose(
            compose_file,
            args.image,
            args.log_level,
            args.mtu,
            args.carrier_bps,
            args.carrier_frame_rate,
        )

        pcap_path = output_root / "fps-link.pcap"
        metadata_path = output_root / "experiment.json"
        logs_path = output_root / "compose.logs"
        capture = None
        logs = ""
        split_epoch = None
        summary = {}
        try:
            compose(base, compose_file, args.project, ["up", "-d", *REQUIRED_SERVICES_BASE])
            wait_for_running_services(
                base,
                compose_file,
                args.project,
                REQUIRED_SERVICES_BASE,
                args.startup_timeout,
            )
            wait_for_tun(base, compose_file, args.project, args.startup_timeout)

            capture_interface = resolve_capture_interface(base, args.project, args.capture_interface)
            capture = start_capture(capture_interface, pcap_path, args.capture_port)
            capture_start_epoch = time.time()

            compose(base, compose_file, args.project, ["up", "-d", "fps-carrier-client"])
            wait_for_service_set(base, compose_file, args.project, args.startup_timeout)
            wait_for_log(
                base,
                compose_file,
                args.project,
                "event=carrier_registered",
                args.startup_timeout,
            )
            split_epoch = time.time()
            wait_for_log(
                base,
                compose_file,
                args.project,
                "event=tun_auto_configured",
                args.startup_timeout,
            )
            wait_for_client_lease(base, compose_file, args.project, args.startup_timeout)

            compose_exec(
                base,
                compose_file,
                args.project,
                "fps-server",
                [
                    "sh",
                    "-lc",
                    "rm -f /tmp/iperf3-server.json /tmp/iperf3-server.err; "
                    "iperf3 -s -B 10.88.0.1 -p 5201 --one-off --json "
                    ">/tmp/iperf3-server.json 2>/tmp/iperf3-server.err &",
                ],
            )
            time.sleep(1.0)
            iperf_command = [
                "iperf3",
                "-c",
                "10.88.0.1",
                "-p",
                "5201",
                "-u",
                "-b",
                args.bandwidth,
                "-t",
                str(args.duration),
                "-l",
                str(args.length),
                "--json",
            ]
            if args.iperf_bidir:
                iperf_command.append("--bidir")
            iperf = compose_exec(base, compose_file, args.project, "fps-client", iperf_command)
            (output_root / "iperf.json").write_text(iperf.stdout, encoding="utf-8")
            iperf_summary = parse_iperf_udp_summaries(iperf.stdout)
            time.sleep(2.0)
            stop_capture(capture)
            capture = None

            logs = compose(
                base,
                compose_file,
                args.project,
                ["logs", "--no-color", *REQUIRED_SERVICES_ALL],
            ).stdout
            logs_path.write_text(logs, encoding="utf-8")
            flow_summary = analyze(root, pcap_path, split_epoch, output_root, args.capture_port)
            summary = {
                "project": args.project,
                "pcap": str(pcap_path),
                "flow_summary": str(output_root / "flow-summary.json"),
                "flow_packets_csv": str(output_root / "flow-packets.csv"),
                "flow_plot_svg": str(output_root / "flow-plot.svg"),
                "capture_start_epoch": capture_start_epoch,
                "capture_interface": capture_interface,
                "upgrade_split_epoch": split_epoch,
                "upgrade_split_after_capture_start_sec": split_epoch - capture_start_epoch,
                "iperf_udp": iperf_summary,
                "flow": flow_summary,
            }
            metadata_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            print(json.dumps(summary, indent=2, sort_keys=True))
        finally:
            stop_capture(capture)
            if logs:
                logs_path.write_text(logs, encoding="utf-8")
            if args.keep_artifacts:
                shutil.copytree(work, output_root / "compose-work", ignore=ignore_private_artifacts)
            compose(
                base,
                compose_file,
                args.project,
                ["down", "-v", "--remove-orphans"],
                check=False,
            )


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
