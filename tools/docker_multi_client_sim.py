#!/usr/bin/env python3

import argparse
import json
import os
import re
import shutil
import sys
import tempfile
import textwrap
import time
from pathlib import Path

from docker_tun_iperf_sim import (
    compose,
    compose_exec,
    docker,
    docker_base,
    generate_server_keypair,
    parse_iperf_udp_summary,
    repo_root,
    stats_have_tun_traffic,
)


CLIENTS = {
    "a": {
        "uuid": "123e4567-e89b-42d3-a456-426614174000",
        "seed": "6101",
        "listen": 7443,
    },
    "b": {
        "uuid": "123e4567-e89b-42d3-a456-426614174001",
        "seed": "6102",
        "listen": 7444,
    },
}
REQUIRED_SERVICES = [
    "fps-carrier-origin",
    "fps-server",
    "fps-client-a",
    "fps-client-b",
    "fps-carrier-client-a",
    "fps-carrier-client-b",
]
SERVER_IP = "10.89.0.1"
LEASE_POOL = "10.89.0.0/29"
SPOOF_IP = "10.89.0.6"


def write_config(
    path: Path,
    *,
    role: str,
    peer: str,
    listen_port: int,
    tun_name: str,
    mtu: int,
    profile_id: str,
    client_uuid: str,
    allowed_client_uuids: list[str],
    server_private_key_base64: str,
    server_public_key_base64: str,
):
    zero_rtt = {
        "enabled": True,
        "profile_id": profile_id,
        "timestamp_window_sec": 30,
        "version": 2,
        "capabilities": 1,
        "max_padding_size": 64,
        "replay_cache_size": 256,
        "trial_decrypt_limit": 1,
        "min_records_before_trial": 1,
        "upgrade_direction": "client_to_server",
    }
    if role == "server":
        target_key = "origin"
        zero_rtt.update(
            {
                "server_private_key_base64": server_private_key_base64,
                "server_public_key_base64": server_public_key_base64,
                "allowed_client_uuids": allowed_client_uuids,
            }
        )
    elif role == "client":
        target_key = "server"
        zero_rtt.update(
            {
                "client_uuid": client_uuid,
                "server_public_key_base64": server_public_key_base64,
            }
        )
    else:
        raise ValueError(role)

    config = {
        "network": {
            "listen": f"0.0.0.0:{listen_port}",
            target_key: peer,
            "read_buffer_size": 65536,
        },
        "security": {"zero_rtt": zero_rtt},
        "codec": {
            "max_frame_payload": 1280,
            "max_frame_padding": 64,
            "allow_fragmentation": True,
        },
        "tun": {
            "enabled": True,
            "name": tun_name,
            "mtu": mtu,
            "max_write_queue_packets": 256,
        },
        "logging": {"level": "info"},
    }
    if role == "server":
        config["tun"].update(
            {
                "lease_pool": LEASE_POOL,
                "server_address": SERVER_IP,
                "lease_file": "/var/lib/fps/leases.json",
                "client_isolation": True,
            }
        )
    else:
        config["tun"]["auto_configure"] = True
    path.write_text(json.dumps(config, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_compose(path: Path, image: str, log_level: str, mtu: int, carrier_bps: int):
    client_services = []
    carrier_services = []
    for suffix, data in CLIENTS.items():
        client_services.append(
            f"""
              fps-client-{suffix}:
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
                  - ./config/client-{suffix}.json:/etc/fps/client.json:ro
                devices:
                  - /dev/net/tun:/dev/net/tun
                cap_add:
                  - NET_ADMIN
                  - NET_BIND_SERVICE
                security_opt:
                  - no-new-privileges:true
            """
        )
        carrier_services.append(
            f"""
              fps-carrier-client-{suffix}:
                image: {image}
                depends_on:
                  - fps-client-{suffix}
                command:
                  - fps_carrier
                  - client
                  - --connect
                  - fps-client-{suffix}:{data["listen"]}
                  - --path
                  - /fps-carrier
                  - --client-bps
                  - "{carrier_bps}"
                  - --frame-rate
                  - "20"
                  - --seed
                  - "{data["seed"]}"
                restart: unless-stopped
                security_opt:
                  - no-new-privileges:true
            """
        )

    rendered_client_services = "\n".join(
        textwrap.indent(textwrap.dedent(item).strip(), "  ") for item in client_services
    )
    rendered_carrier_services = "\n".join(
        textwrap.indent(textwrap.dedent(item).strip(), "  ") for item in carrier_services
    )

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
                  FPS_TUN_ADDRESS: {SERVER_IP}/29
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
            """
        ).rstrip()
        + "\n"
        + rendered_client_services
        + "\n"
        + rendered_carrier_services
        + "\n",
        encoding="utf-8",
    )


def wait_for_service_set(base: list[str], compose_file: Path, project: str, timeout: float):
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        result = compose(base, compose_file, project, ["ps", "--services", "--status", "running"])
        running = set(result.stdout.split())
        if set(REQUIRED_SERVICES).issubset(running):
            return
        last = result.stdout
        time.sleep(0.5)
    raise RuntimeError(f"services did not all become running: {last}")


def lease_ip_from_addr_output(output: str) -> str | None:
    match = re.search(r"\binet\s+(10\.89\.0\.[0-9]+)/29\b", output)
    if not match:
        return None
    return match.group(1)


def wait_for_client_leases(
    base: list[str],
    compose_file: Path,
    project: str,
    timeout: float,
) -> dict[str, str]:
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        leases = {}
        for suffix in CLIENTS:
            service = f"fps-client-{suffix}"
            result = compose_exec(
                base,
                compose_file,
                project,
                service,
                ["ip", "addr", "show", "dev", "fpsc0"],
                check=False,
            )
            last[service] = result.stdout
            if result.returncode == 0:
                lease_ip = lease_ip_from_addr_output(result.stdout)
                if lease_ip:
                    leases[suffix] = lease_ip
        if len(leases) == len(CLIENTS) and len(set(leases.values())) == len(CLIENTS):
            return leases
        time.sleep(0.5)
    raise RuntimeError(f"clients did not receive distinct leases: {last}")


def wait_for_log_count(
    base: list[str],
    compose_file: Path,
    project: str,
    pattern: str,
    count: int,
    timeout: float,
) -> str:
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        result = compose(
            base,
            compose_file,
            project,
            ["logs", "--no-color", "fps-server", "fps-client-a", "fps-client-b"],
        )
        last = result.stdout
        if last.count(pattern) >= count:
            return last
        time.sleep(0.5)
    raise RuntimeError(f"timed out waiting for {count} log entries {pattern!r}:\n{last}")


def start_iperf_server(
    base: list[str],
    compose_file: Path,
    project: str,
    service: str,
    bind_ip: str,
    port: int,
):
    compose_exec(
        base,
        compose_file,
        project,
        service,
        [
            "sh",
            "-lc",
            f"rm -f /tmp/iperf3-{port}.json /tmp/iperf3-{port}.err; "
            f"iperf3 -s -B {bind_ip} -p {port} --one-off --json "
            f">/tmp/iperf3-{port}.json 2>/tmp/iperf3-{port}.err &",
        ],
    )
    time.sleep(1.0)


def run_udp_client(
    base: list[str],
    compose_file: Path,
    project: str,
    service: str,
    target_ip: str,
    port: int,
    bandwidth: str,
    duration: int,
    length: int,
) -> dict:
    result = compose_exec(
        base,
        compose_file,
        project,
        service,
        [
            "iperf3",
            "-c",
            target_ip,
            "-p",
            str(port),
            "-u",
            "-b",
            bandwidth,
            "-t",
            str(duration),
            "-l",
            str(length),
            "--json",
        ],
    )
    return parse_iperf_udp_summary(result.stdout)


def send_spoofed_udp(
    base: list[str],
    compose_file: Path,
    project: str,
    client_suffix: str,
):
    service = f"fps-client-{client_suffix}"
    compose_exec(
        base,
        compose_file,
        project,
        service,
        ["ip", "addr", "add", f"{SPOOF_IP}/32", "dev", "fpsc0"],
        check=False,
    )
    compose_exec(
        base,
        compose_file,
        project,
        service,
        [
            "python3",
            "-c",
            (
                "import socket;"
                f"s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM);"
                f"s.bind(('{SPOOF_IP}', 0));"
                f"s.sendto(b'fps-spoof-check', ('{SERVER_IP}', 9));"
                "s.close()"
            ),
        ],
    )


def collect_logs(base: list[str], compose_file: Path, project: str) -> str:
    return compose(
        base,
        compose_file,
        project,
        ["logs", "--no-color", *REQUIRED_SERVICES],
    ).stdout


def extract_session_stats(logs: str) -> list[str]:
    return [line for line in logs.splitlines() if "event=session_stats" in line]


def ignore_private_artifacts(_directory, names):
    return {name for name in names if name.endswith(".key")}


def main():
    parser = argparse.ArgumentParser(
        description="Run a Docker FPS server with two leased clients and TUN probes."
    )
    parser.add_argument("--image", default=os.environ.get("FPS_DOCKER_IMAGE", "fps:local"))
    parser.add_argument("--build", action="store_true", help="Build the image before running.")
    parser.add_argument("--sudo", action="store_true", help="Use sudo -n docker.")
    parser.add_argument("--project", default=f"fps-multi-client-{os.getpid()}")
    parser.add_argument("--duration", type=int, default=3)
    parser.add_argument("--bandwidth", default="250K")
    parser.add_argument("--length", type=int, default=512)
    parser.add_argument("--mtu", type=int, default=1280)
    parser.add_argument("--carrier-bps", type=int, default=262144)
    parser.add_argument("--log-level", default="debug")
    parser.add_argument("--startup-timeout", type=float, default=60.0)
    parser.add_argument("--keep-artifacts", action="store_true")
    args = parser.parse_args()

    if args.duration <= 0:
        raise ValueError("--duration must be positive")
    if args.length <= 0:
        raise ValueError("--length must be positive")
    if args.mtu <= 0:
        raise ValueError("--mtu must be positive")
    if args.carrier_bps <= 0:
        raise ValueError("--carrier-bps must be positive")

    base = docker_base(args.sudo)
    root = repo_root()
    if args.build:
        docker(base, ["build", "-t", args.image, str(root)])

    with tempfile.TemporaryDirectory(prefix="fps-docker-multi-client-") as tmp:
        work = Path(tmp)
        config_dir = work / "config"
        config_dir.mkdir()
        server_keys = generate_server_keypair(base, args.image)
        allowed_uuids = [data["uuid"] for data in CLIENTS.values()]
        profile_id = "docker-multi-client-v2"
        write_config(
            config_dir / "server.json",
            role="server",
            peer="fps-carrier-origin:9443",
            listen_port=8443,
            tun_name="fpss0",
            mtu=args.mtu,
            profile_id=profile_id,
            client_uuid="",
            allowed_client_uuids=allowed_uuids,
            server_private_key_base64=server_keys["server_private_key_base64"],
            server_public_key_base64=server_keys["server_public_key_base64"],
        )
        for suffix, data in CLIENTS.items():
            write_config(
                config_dir / f"client-{suffix}.json",
                role="client",
                peer="fps-server:8443",
                listen_port=data["listen"],
                tun_name="fpsc0",
                mtu=args.mtu,
                profile_id=profile_id,
                client_uuid=data["uuid"],
                allowed_client_uuids=[],
                server_private_key_base64=server_keys["server_private_key_base64"],
                server_public_key_base64=server_keys["server_public_key_base64"],
            )
        compose_file = work / "compose.yml"
        write_compose(compose_file, args.image, args.log_level, args.mtu, args.carrier_bps)

        summary = {}
        logs = ""
        try:
            compose(base, compose_file, args.project, ["up", "-d"])
            wait_for_service_set(base, compose_file, args.project, args.startup_timeout)
            wait_for_log_count(
                base,
                compose_file,
                args.project,
                "event=carrier_registered",
                len(CLIENTS),
                args.startup_timeout,
            )
            wait_for_log_count(
                base,
                compose_file,
                args.project,
                "event=tun_lease_assigned",
                len(CLIENTS),
                args.startup_timeout,
            )
            leases = wait_for_client_leases(base, compose_file, args.project, args.startup_timeout)
            summary["leases"] = leases

            server_to_client = {}
            for index, (suffix, lease_ip) in enumerate(sorted(leases.items()), start=1):
                port = 5310 + index
                start_iperf_server(
                    base,
                    compose_file,
                    args.project,
                    f"fps-client-{suffix}",
                    lease_ip,
                    port,
                )
                server_to_client[suffix] = run_udp_client(
                    base,
                    compose_file,
                    args.project,
                    "fps-server",
                    lease_ip,
                    port,
                    args.bandwidth,
                    args.duration,
                    args.length,
                )
            summary["server_to_client_udp"] = server_to_client

            client_to_server = {}
            for index, suffix in enumerate(sorted(leases), start=1):
                port = 5410 + index
                start_iperf_server(
                    base,
                    compose_file,
                    args.project,
                    "fps-server",
                    SERVER_IP,
                    port,
                )
                client_to_server[suffix] = run_udp_client(
                    base,
                    compose_file,
                    args.project,
                    f"fps-client-{suffix}",
                    SERVER_IP,
                    port,
                    args.bandwidth,
                    args.duration,
                    args.length,
                )
            summary["client_to_server_udp"] = client_to_server

            send_spoofed_udp(base, compose_file, args.project, "a")
            wait_for_log_count(
                base,
                compose_file,
                args.project,
                "detail=ignored_spoofed_tun_source",
                1,
                args.startup_timeout,
            )
            summary["spoof_drop_event"] = True

            port = 5501
            start_iperf_server(base, compose_file, args.project, "fps-server", SERVER_IP, port)
            summary["post_spoof_valid_udp"] = run_udp_client(
                base,
                compose_file,
                args.project,
                "fps-client-a",
                SERVER_IP,
                port,
                args.bandwidth,
                args.duration,
                args.length,
            )

            running_after = compose(
                base, compose_file, args.project, ["ps", "--services", "--status", "running"]
            ).stdout.split()
            summary["services_running_after_probes"] = sorted(running_after)
            missing = sorted(set(REQUIRED_SERVICES) - set(running_after))
            if missing:
                raise RuntimeError(f"services exited during probes: {missing}")

            compose(base, compose_file, args.project, ["stop", "-t", "5", "fps-carrier-client-a"])
            compose(base, compose_file, args.project, ["stop", "-t", "5", "fps-carrier-client-b"])
            time.sleep(1.0)
            logs = collect_logs(base, compose_file, args.project)
            stats_lines = extract_session_stats(logs)
            summary["fps_session_stats_lines"] = len(stats_lines)
            summary["fps_tun_stats_nonzero"] = stats_have_tun_traffic(stats_lines)
            if len(stats_lines) < len(CLIENTS):
                raise RuntimeError("FPS logs did not contain session_stats for both clients")
            if not summary["fps_tun_stats_nonzero"]:
                raise RuntimeError("FPS session_stats did not contain non-zero TUN frame counters")

            print(json.dumps(summary, indent=2, sort_keys=True))
        finally:
            (work / "compose.logs").write_text(logs, encoding="utf-8")
            if args.keep_artifacts:
                persistent = Path.cwd() / "captures" / args.project
                if persistent.exists():
                    shutil.rmtree(persistent)
                persistent.parent.mkdir(parents=True, exist_ok=True)
                shutil.copytree(work, persistent, ignore=ignore_private_artifacts)
                print(f"artifacts={persistent}")
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
        raise SystemExit(1)
