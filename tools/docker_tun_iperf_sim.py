#!/usr/bin/env python3

import argparse
import json
import os
import shutil
import sys
import tempfile
import textwrap
import time
from pathlib import Path

from fps_docker_common import (
    compose,
    compose_exec,
    docker,
    docker_base,
    extract_session_stats,
    generate_server_keypair,
    ignore_private_artifacts,
    parse_iperf_udp_summary,
    repo_root,
    stats_have_tun_traffic,
    wait_for_log as wait_for_compose_log,
    wait_for_services,
    write_json,
)


REQUIRED_SERVICES = [
    "fps-carrier-origin",
    "fps-server",
    "fps-client",
    "fps-carrier-client",
]
CLIENT_UUID = "123e4567-e89b-42d3-a456-426614174000"


def write_config(
    path: Path,
    role: str,
    peer: str,
    tun_name: str,
    mtu: int,
    profile_id: str,
    client_uuid: str,
    server_private_key_base64: str,
    server_public_key_base64: str,
):
    if role == "server":
        target_key = "origin"
        zero_rtt_keys = {
            "server_private_key_base64": server_private_key_base64,
            "server_public_key_base64": server_public_key_base64,
            "allowed_client_uuids": [client_uuid],
        }
    elif role == "client":
        target_key = "server"
        zero_rtt_keys = {
            "client_uuid": client_uuid,
            "server_public_key_base64": server_public_key_base64,
        }
    else:
        raise ValueError(role)

    config = {
        "network": {
            "listen": f"0.0.0.0:{8443 if role == 'server' else 7443}",
            target_key: peer,
            "read_buffer_size": 65536,
        },
        "security": {
            "zero_rtt": {
                "enabled": True,
                "profile_id": profile_id,
                **zero_rtt_keys,
                "version": 5,
                "capabilities": 1,
                "max_padding_size": 64,
                "min_records_before_trial": 1,
                "upgrade_direction": "client_to_server",
            }
        },
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
        "logging": {
            "level": "info",
        },
    }
    if role == "server":
        config["tun"].update(
            {
                "lease_pool": "10.88.0.0/30",
                "server_address": "10.88.0.1",
                "lease_file": "/var/lib/fps/leases.json",
                "client_isolation": True,
            }
        )
    else:
        config["tun"]["auto_configure"] = True
    write_json(path, config)


def write_compose(path: Path, image: str, log_level: str, mtu: int, carrier_bps: int):
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
                  - "20"
                  - --seed
                  - "4242"
                restart: unless-stopped
                security_opt:
                  - no-new-privileges:true
            """
        ).strip()
        + "\n",
        encoding="utf-8",
    )


def wait_for_service_set(base: list[str], compose_file: Path, project: str, timeout: float):
    wait_for_services(base, compose_file, project, REQUIRED_SERVICES, timeout)


def wait_for_tun(base: list[str], compose_file: Path, project: str, timeout: float):
    deadline = time.monotonic() + timeout
    probes = [("fps-server", "fpss0"), ("fps-client", "fpsc0")]
    last = ""
    while time.monotonic() < deadline:
        ready = True
        for service, tun in probes:
            result = compose_exec(
                base,
                compose_file,
                project,
                service,
                ["ip", "addr", "show", "dev", tun],
                check=False,
            )
            if result.returncode != 0:
                ready = False
                last = result.stdout
                break
        if ready:
            return
        time.sleep(0.5)
    raise RuntimeError(f"TUN interfaces did not become ready: {last}")


def wait_for_client_lease(base: list[str], compose_file: Path, project: str, timeout: float):
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        result = compose_exec(
            base,
            compose_file,
            project,
            "fps-client",
            ["ip", "addr", "show", "dev", "fpsc0"],
            check=False,
        )
        last = result.stdout
        if result.returncode == 0 and "10.88.0.2/30" in result.stdout:
            return
        time.sleep(0.5)
    raise RuntimeError(f"client did not apply server-assigned lease: {last}")


def wait_for_log(
    base: list[str],
    compose_file: Path,
    project: str,
    pattern: str,
    timeout: float,
):
    return wait_for_compose_log(
        base,
        compose_file,
        project,
        pattern,
        timeout,
        ["fps-server", "fps-client"],
    )


def main():
    parser = argparse.ArgumentParser(
        description="Run a Docker FPS client/server TUN UDP iperf3 simulation."
    )
    parser.add_argument("--image", default=os.environ.get("FPS_DOCKER_IMAGE", "fps:local"))
    parser.add_argument("--build", action="store_true", help="Build the image before running.")
    parser.add_argument("--sudo", action="store_true", help="Use sudo -n docker.")
    parser.add_argument("--project", default=f"fps-iperf-{os.getpid()}")
    parser.add_argument("--duration", type=int, default=10)
    parser.add_argument("--bandwidth", default="250K")
    parser.add_argument("--length", type=int, default=512)
    parser.add_argument("--mtu", type=int, default=1280)
    parser.add_argument("--carrier-bps", type=int, default=262144)
    parser.add_argument("--log-level", default="debug")
    parser.add_argument("--startup-timeout", type=float, default=45.0)
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

    with tempfile.TemporaryDirectory(prefix="fps-docker-tun-iperf-") as tmp:
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
            "docker-tun-iperf-v5",
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
            "docker-tun-iperf-v5",
            CLIENT_UUID,
            server_keys["server_private_key_base64"],
            server_keys["server_public_key_base64"],
        )
        compose_file = work / "compose.yml"
        write_compose(compose_file, args.image, args.log_level, args.mtu, args.carrier_bps)

        summary = {}
        logs = ""
        try:
            compose(base, compose_file, args.project, ["up", "-d"])
            wait_for_service_set(base, compose_file, args.project, args.startup_timeout)
            wait_for_tun(base, compose_file, args.project, args.startup_timeout)
            wait_for_log(
                base,
                compose_file,
                args.project,
                "event=carrier_registered",
                args.startup_timeout,
            )
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
            iperf = compose_exec(
                base,
                compose_file,
                args.project,
                "fps-client",
                [
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
                ],
            )
            summary["udp"] = parse_iperf_udp_summary(iperf.stdout)

            running_after = compose(
                base, compose_file, args.project, ["ps", "--services", "--status", "running"]
            ).stdout.split()
            summary["services_running_after_iperf"] = sorted(running_after)
            missing = sorted(set(REQUIRED_SERVICES) - set(running_after))
            if missing:
                raise RuntimeError(f"services exited during benchmark: {missing}")

            compose(base, compose_file, args.project, ["stop", "-t", "5", "fps-carrier-client"])
            time.sleep(1.0)
            logs = compose(
                base,
                compose_file,
                args.project,
                ["logs", "--no-color", *REQUIRED_SERVICES],
            ).stdout
            stats_lines = extract_session_stats(logs)
            summary["fps_session_stats_lines"] = len(stats_lines)
            summary["fps_tun_stats_nonzero"] = stats_have_tun_traffic(stats_lines)
            if not stats_lines:
                raise RuntimeError("FPS logs did not contain event=session_stats")
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
                print(f"artifacts={persistent}", file=sys.stderr)
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
