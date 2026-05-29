#!/usr/bin/env python3

import argparse
import json
import os
import shlex
import shutil
import sys
import tempfile
import textwrap
import time
from pathlib import Path

from docker_multi_client_sim import (
    CLIENTS,
    REQUIRED_SERVICES as MULTI_CLIENT_SERVICES,
    SERVER_IP,
    collect_logs,
    ignore_private_artifacts,
    lease_ip_from_addr_output,
    run_udp_client,
    send_spoofed_udp,
    start_iperf_server,
    wait_for_log_count,
    write_compose as write_multi_client_compose,
    write_config,
)
from docker_tun_iperf_sim import (
    compose,
    compose_exec,
    docker,
    docker_base,
    generate_server_keypair,
    repo_root,
)


HTTP_PORT = 18081
SOCKS_PORT = 1080
SOCKS_HTTP_ORIGIN = "soak-http-origin"
SOCKS_PROXY_SERVICE = "fps-dante-proxy"


def build_images(base: list[str], root: Path, image: str, proxy_image: str | None):
    docker(base, ["build", "-t", image, str(root)])
    if proxy_image is not None:
        docker(
            base,
            [
                "build",
                "-f",
                str(root / "examples/docker/proxy-dante/Dockerfile"),
                "--build-arg",
                f"FPS_BASE_IMAGE={image}",
                "-t",
                proxy_image,
                str(root),
            ],
        )


def patch_config(path: Path, status_socket: str, write_queue_bytes: int | None):
    config = json.loads(path.read_text(encoding="utf-8"))
    config["ops"] = {"status_socket": status_socket}
    if write_queue_bytes is not None:
        config.setdefault("limits", {})["max_session_write_queue_bytes"] = write_queue_bytes
    path.write_text(json.dumps(config, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def append_socks_overlay_services(path: Path, image: str, proxy_image: str):
    with path.open("a", encoding="utf-8") as handle:
        handle.write(
            f"""

  {SOCKS_HTTP_ORIGIN}:
    image: {image}
    command:
      - python3
      - -m
      - http.server
      - "18080"
      - --bind
      - 0.0.0.0
    restart: unless-stopped
    security_opt:
      - no-new-privileges:true

  {SOCKS_PROXY_SERVICE}:
    image: {proxy_image}
    depends_on:
      - fps-server
    network_mode: "service:fps-server"
    restart: unless-stopped
    environment:
      FPS_SOCKS_LISTEN_ADDRESS: {SERVER_IP}
      FPS_SOCKS_PORT: "{SOCKS_PORT}"
      FPS_SOCKS_ALLOWED_CIDR: 10.89.0.0/29
      FPS_SOCKS_EXTERNAL_INTERFACE: eth0
    security_opt:
      - no-new-privileges:true
"""
        )


def required_services(with_socks_overlay: bool) -> list[str]:
    services = list(MULTI_CLIENT_SERVICES)
    if with_socks_overlay:
        services.extend([SOCKS_HTTP_ORIGIN, SOCKS_PROXY_SERVICE])
    return services


def wait_for_services(
    base: list[str],
    compose_file: Path,
    project: str,
    services: list[str],
    timeout: float,
):
    deadline = time.monotonic() + timeout
    expected = set(services)
    last = ""
    while time.monotonic() < deadline:
        result = compose(base, compose_file, project, ["ps", "--services", "--status", "running"])
        running = set(result.stdout.split())
        if expected.issubset(running):
            return
        last = result.stdout
        time.sleep(0.5)
    raise RuntimeError(f"services did not all become running: expected={services!r} running={last!r}")


def query_status(base: list[str], compose_file: Path, project: str, service: str) -> dict:
    binary = "fps_server" if service == "fps-server" else "fps_client"
    completed = compose_exec(
        base,
        compose_file,
        project,
        service,
        [
            binary,
            "--status",
            "--config",
            "/etc/fps/server.json" if service == "fps-server" else "/etc/fps/client.json",
        ],
    )
    return json.loads(completed.stdout)


def wait_for_status(
    base: list[str],
    compose_file: Path,
    project: str,
    service: str,
    predicate,
    timeout: float,
    description: str,
) -> dict:
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        status = query_status(base, compose_file, project, service)
        last = status
        if predicate(status):
            return status
        time.sleep(0.5)
    raise RuntimeError(f"timed out waiting for status condition {description}: {last!r}")


def wait_for_client_leases(
    base: list[str],
    compose_file: Path,
    project: str,
    suffixes: list[str],
    timeout: float,
) -> dict[str, str]:
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        leases = {}
        for suffix in suffixes:
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
        if len(leases) == len(suffixes) and len(set(leases.values())) == len(suffixes):
            return leases
        time.sleep(0.5)
    raise RuntimeError(f"clients did not receive expected distinct leases: {last}")


def tun_counter(status: dict, section: str, name: str) -> int:
    value = status.get("tun", {}).get(section, {}).get(name, 0)
    return int(value or 0)


def session_counter(status: dict, name: str) -> int:
    value = status.get("sessions", {}).get(name, 0)
    return int(value or 0)


def auth_counter(status: dict, name: str) -> int:
    value = status.get("auth", {}).get(name, 0)
    return int(value or 0)


def close_reasons(status: dict) -> set[str]:
    reasons = set()
    recent = status.get("sessions", {}).get("recent_closed", [])
    if not isinstance(recent, list):
        return reasons
    for item in recent:
        if not isinstance(item, dict):
            continue
        close = item.get("close", {})
        if isinstance(close, dict) and isinstance(close.get("reason"), str):
            reasons.add(close["reason"])
    return reasons


def start_http_server(base: list[str], compose_file: Path, project: str):
    compose_exec(
        base,
        compose_file,
        project,
        "fps-server",
        [
            "sh",
            "-lc",
            "if [ -f /tmp/fps-soak-http.pid ]; then "
            "kill \"$(cat /tmp/fps-soak-http.pid)\" >/dev/null 2>&1 || true; "
            "rm -f /tmp/fps-soak-http.pid; "
            "fi; "
            f"python3 -m http.server {HTTP_PORT} --bind {SERVER_IP} "
            ">/tmp/fps-soak-http.log 2>&1 & "
            "echo $! >/tmp/fps-soak-http.pid",
        ],
    )
    time.sleep(0.5)


def start_http_probe_loop(
    base: list[str],
    compose_file: Path,
    project: str,
    service: str,
    duration: int,
    marker_path: str,
):
    code = textwrap.dedent(
        f"""
        import pathlib
        import socket
        import time

        deadline = time.monotonic() + {duration}
        count = 0
        last_error = ""
        while time.monotonic() < deadline:
            try:
                sock = socket.create_connection(({SERVER_IP!r}, {HTTP_PORT}), 3.0)
                sock.sendall(
                    b"GET / HTTP/1.1\\r\\n"
                    b"Host: fps-soak\\r\\n"
                    b"Connection: close\\r\\n\\r\\n"
                )
                body = b""
                while True:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    body += chunk
                sock.close()
                if b"200 OK" in body:
                    count += 1
                else:
                    last_error = body[:120].decode("latin1", "replace")
            except Exception as error:
                last_error = str(error)
            time.sleep(0.5)
        pathlib.Path({marker_path!r}).write_text(f"{{count}}\\n{{last_error}}\\n", encoding="utf-8")
        """
    )
    compose_exec(
        base,
        compose_file,
        project,
        service,
        [
            "sh",
            "-lc",
            f"rm -f {shlex.quote(marker_path)}; "
            f"python3 -c {shlex.quote(code)} >/tmp/fps-soak-http-probe.log 2>&1 &",
        ],
    )


def read_probe_count(
    base: list[str],
    compose_file: Path,
    project: str,
    service: str,
    marker_path: str,
) -> int:
    completed = compose_exec(
        base,
        compose_file,
        project,
        service,
        ["sh", "-lc", f"cat {shlex.quote(marker_path)}"],
    )
    first_line = completed.stdout.splitlines()[0]
    return int(first_line)


def wait_for_socks(base: list[str], compose_file: Path, project: str, timeout: float):
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        result = compose_exec(
            base,
            compose_file,
            project,
            "fps-client-a",
            [
                "python3",
                "-c",
                (
                    "import socket; "
                    f"s=socket.create_connection(('{SERVER_IP}',{SOCKS_PORT}),1); "
                    "s.close()"
                ),
            ],
            check=False,
        )
        last = result.stdout
        if result.returncode == 0:
            return
        time.sleep(0.5)
    raise RuntimeError(f"SOCKS listener did not become reachable over TUN: {last}")


def run_socks_probe(base: list[str], compose_file: Path, project: str) -> str:
    code = f"""
import socket
import struct

def recvn(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("unexpected EOF from SOCKS server")
        data += chunk
    return data

sock = socket.create_connection(("{SERVER_IP}", {SOCKS_PORT}), 5)
sock.sendall(b"\\x05\\x01\\x00")
if recvn(sock, 2) != b"\\x05\\x00":
    raise RuntimeError("SOCKS no-auth negotiation failed")
host = b"{SOCKS_HTTP_ORIGIN}"
sock.sendall(b"\\x05\\x01\\x00\\x03" + bytes([len(host)]) + host + struct.pack("!H", 18080))
reply = recvn(sock, 4)
if reply[1] != 0:
    raise RuntimeError(f"SOCKS connect failed with status {{reply[1]}}")
if reply[3] == 1:
    recvn(sock, 4)
elif reply[3] == 3:
    recvn(sock, recvn(sock, 1)[0])
elif reply[3] == 4:
    recvn(sock, 16)
else:
    raise RuntimeError("invalid SOCKS bind address type")
recvn(sock, 2)
sock.sendall(b"GET / HTTP/1.1\\r\\nHost: {SOCKS_HTTP_ORIGIN}\\r\\nConnection: close\\r\\n\\r\\n")
body = b""
while True:
    chunk = sock.recv(4096)
    if not chunk:
        break
    body += chunk
if b"200 OK" not in body:
    raise RuntimeError(body[:200].decode("latin1", "replace"))
print("socks_overlay_ok")
"""
    completed = compose_exec(
        base,
        compose_file,
        project,
        "fps-client-a",
        ["python3", "-c", code],
    )
    return completed.stdout.strip()


def assert_no_secret_status_leak(statuses: list[dict]):
    text = json.dumps(statuses, sort_keys=True)
    forbidden = ["client_uuid", "allowed_client_uuids", "server_private_key", "ClientID"]
    leaked = [item for item in forbidden if item in text]
    if leaked:
        raise RuntimeError(f"status leaked sensitive fields: {leaked}")


def run_sustained_mixed_traffic(
    base: list[str],
    compose_file: Path,
    project: str,
    duration: int,
    bandwidth: str,
    length: int,
) -> dict:
    start_http_server(base, compose_file, project)
    marker = "/tmp/fps-soak-http-probe-count"
    start_http_probe_loop(base, compose_file, project, "fps-client-a", duration, marker)
    port = 5801
    start_iperf_server(base, compose_file, project, "fps-server", SERVER_IP, port)
    udp = run_udp_client(
        base,
        compose_file,
        project,
        "fps-client-a",
        SERVER_IP,
        port,
        bandwidth,
        duration,
        length,
    )
    count = read_probe_count(base, compose_file, project, "fps-client-a", marker)
    if count <= 0:
        raise RuntimeError("mixed TCP probe did not complete during UDP traffic")
    return {"udp": udp, "http_probe_count": count}


def run_server_to_client_routing(
    base: list[str],
    compose_file: Path,
    project: str,
    leases: dict[str, str],
    duration: int,
    bandwidth: str,
    length: int,
) -> dict:
    results = {}
    for index, (suffix, lease_ip) in enumerate(sorted(leases.items()), start=1):
        port = 5900 + index
        start_iperf_server(base, compose_file, project, f"fps-client-{suffix}", lease_ip, port)
        results[suffix] = run_udp_client(
            base,
            compose_file,
            project,
            "fps-server",
            lease_ip,
            port,
            bandwidth,
            duration,
            length,
        )
    return results


def run_spoof_drop_liveness(
    base: list[str],
    compose_file: Path,
    project: str,
    startup_timeout: float,
    duration: int,
    bandwidth: str,
    length: int,
) -> dict:
    send_spoofed_udp(base, compose_file, project, "a")
    wait_for_log_count(
        base,
        compose_file,
        project,
        "detail=ignored_spoofed_tun_source",
        1,
        startup_timeout,
    )
    port = 6001
    start_iperf_server(base, compose_file, project, "fps-server", SERVER_IP, port)
    valid = run_udp_client(
        base,
        compose_file,
        project,
        "fps-client-a",
        SERVER_IP,
        port,
        bandwidth,
        duration,
        length,
    )
    server_status = query_status(base, compose_file, project, "fps-server")
    spoof_drops = tun_counter(server_status, "session_manager_events", "ignored_spoofed_tun_source")
    if spoof_drops <= 0:
        raise RuntimeError(f"server status did not report spoof drop: {server_status!r}")
    return {"spoof_drops": spoof_drops, "post_spoof_udp": valid}


def send_udp_packet_from_client(
    base: list[str],
    compose_file: Path,
    project: str,
    suffix: str,
    source_ip: str,
):
    code = (
        "import socket;"
        "sock=socket.socket(socket.AF_INET, socket.SOCK_DGRAM);"
        f"sock.bind(({source_ip!r}, 0));"
        f"sock.sendto(b'fps-carrier-loss-check', ({SERVER_IP!r}, 9));"
        "sock.close()"
    )
    compose_exec(base, compose_file, project, f"fps-client-{suffix}", ["python3", "-c", code])


def run_carrier_loss_recovery(
    base: list[str],
    compose_file: Path,
    project: str,
    leases: dict[str, str],
    expected_services: list[str],
    expected_registration_count: int,
    startup_timeout: float,
    duration: int,
    bandwidth: str,
    length: int,
) -> dict:
    before = query_status(base, compose_file, project, "fps-client-a")
    before_no_carrier = tun_counter(before, "session_errors", "no_carrier_session")

    compose(base, compose_file, project, ["stop", "-t", "5", "fps-carrier-client-a"])
    wait_for_status(
        base,
        compose_file,
        project,
        "fps-client-a",
        lambda status: session_counter(status, "carriers_current") == 0,
        startup_timeout,
        "client-a carrier count to reach zero after carrier stop",
    )
    send_udp_packet_from_client(base, compose_file, project, "a", leases["a"])
    after_loss = wait_for_status(
        base,
        compose_file,
        project,
        "fps-client-a",
        lambda status: tun_counter(status, "session_errors", "no_carrier_session")
        > before_no_carrier,
        startup_timeout,
        "no_carrier_session counter to increment",
    )

    compose(base, compose_file, project, ["start", "fps-carrier-client-a"])
    wait_for_services(
        base,
        compose_file,
        project,
        expected_services,
        startup_timeout,
    )
    wait_for_status(
        base,
        compose_file,
        project,
        "fps-client-a",
        lambda status: session_counter(status, "carriers_current") >= 1,
        startup_timeout,
        "client-a carrier to recover",
    )
    wait_for_log_count(
        base,
        compose_file,
        project,
        "event=carrier_registered",
        expected_registration_count,
        startup_timeout,
    )

    port = 6101
    start_iperf_server(base, compose_file, project, "fps-server", SERVER_IP, port)
    recovered = run_udp_client(
        base,
        compose_file,
        project,
        "fps-client-a",
        SERVER_IP,
        port,
        bandwidth,
        duration,
        length,
    )
    return {
        "no_carrier_session_before": before_no_carrier,
        "no_carrier_session_after": tun_counter(after_loss, "session_errors", "no_carrier_session"),
        "recovered_udp": recovered,
    }


def run_backpressure_stress(
    base: list[str],
    compose_file: Path,
    project: str,
    duration: int,
    bandwidth: str,
    length: int,
    startup_timeout: float,
    require_event: bool,
) -> dict:
    before_statuses = [
        query_status(base, compose_file, project, service)
        for service in ("fps-server", "fps-client-a", "fps-client-b")
    ]
    before_write_queue = sum(
        tun_counter(status, "session_errors", "write_queue_full") for status in before_statuses
    )
    before_reasons = set().union(*(close_reasons(status) for status in before_statuses))

    port = 6201
    start_iperf_server(base, compose_file, project, "fps-server", SERVER_IP, port)
    udp = run_udp_client(
        base,
        compose_file,
        project,
        "fps-client-b",
        SERVER_IP,
        port,
        bandwidth,
        duration,
        length,
    )
    wait_for_services(base, compose_file, project, MULTI_CLIENT_SERVICES, startup_timeout)
    after_statuses = [
        query_status(base, compose_file, project, service)
        for service in ("fps-server", "fps-client-a", "fps-client-b")
    ]
    after_write_queue = sum(
        tun_counter(status, "session_errors", "write_queue_full") for status in after_statuses
    )
    after_reasons = set().union(*(close_reasons(status) for status in after_statuses))
    observed = after_write_queue > before_write_queue or "write_queue_full" in (
        after_reasons - before_reasons
    )
    if require_event and not observed:
        raise RuntimeError(
            "backpressure stress did not produce write_queue_full status/close metadata; "
            "increase --stress-bandwidth or lower --write-queue-bytes"
        )
    return {
        "observed_write_queue_full": observed,
        "udp": udp,
        "write_queue_full_before": before_write_queue,
        "write_queue_full_after": after_write_queue,
        "close_reasons": sorted(after_reasons),
    }


def main():
    parser = argparse.ArgumentParser(
        description="Run a Docker FPS resilience soak over leased TUN clients."
    )
    parser.add_argument("--image", default=os.environ.get("FPS_DOCKER_IMAGE", "fps:local"))
    parser.add_argument(
        "--proxy-image",
        default=os.environ.get("FPS_DANTE_PROXY_IMAGE", "fps-dante-proxy:local"),
    )
    parser.add_argument("--build", action="store_true", help="Build Docker images before running.")
    parser.add_argument("--sudo", action="store_true", help="Use sudo -n docker.")
    parser.add_argument("--project", default=f"fps-resilience-soak-{os.getpid()}")
    parser.add_argument("--duration", type=int, default=60)
    parser.add_argument("--bandwidth", default="500K")
    parser.add_argument("--length", type=int, default=512)
    parser.add_argument("--mtu", type=int, default=1280)
    parser.add_argument("--carrier-bps", type=int, default=262144)
    parser.add_argument("--log-level", default="debug")
    parser.add_argument("--startup-timeout", type=float, default=75.0)
    parser.add_argument("--clients", type=int, choices=(1, 2), default=2)
    parser.add_argument("--with-socks-overlay", action="store_true")
    parser.add_argument("--stress-backpressure", action="store_true")
    parser.add_argument("--stress-bandwidth", default="5M")
    parser.add_argument("--require-backpressure-event", action="store_true")
    parser.add_argument("--write-queue-bytes", type=int, default=None)
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
    if args.write_queue_bytes is not None and args.write_queue_bytes <= 0:
        raise ValueError("--write-queue-bytes must be positive")

    base = docker_base(args.sudo)
    root = repo_root()
    if args.build:
        build_images(
            base,
            root,
            args.image,
            args.proxy_image if args.with_socks_overlay else None,
        )

    with tempfile.TemporaryDirectory(prefix="fps-docker-resilience-soak-") as tmp:
        work = Path(tmp)
        config_dir = work / "config"
        config_dir.mkdir()
        server_keys = generate_server_keypair(base, args.image)
        allowed_uuids = [data["uuid"] for data in CLIENTS.values()]
        profile_id = "docker-resilience-soak-v4"
        write_queue_bytes = args.write_queue_bytes

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
        patch_config(config_dir / "server.json", "/run/fps/server.status", write_queue_bytes)
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
            patch_config(
                config_dir / f"client-{suffix}.json",
                "/run/fps/client.status",
                write_queue_bytes,
            )

        compose_file = work / "compose.yml"
        write_multi_client_compose(
            compose_file,
            args.image,
            args.log_level,
            args.mtu,
            args.carrier_bps,
        )
        if args.with_socks_overlay:
            append_socks_overlay_services(compose_file, args.image, args.proxy_image)

        services = required_services(args.with_socks_overlay)
        if args.clients == 1:
            services = [service for service in services if not service.endswith("-b")]

        summary = {}
        logs = ""
        try:
            compose(base, compose_file, args.project, ["up", "-d", *services])
            wait_for_services(base, compose_file, args.project, services, args.startup_timeout)
            wait_for_log_count(
                base,
                compose_file,
                args.project,
                "event=carrier_registered",
                args.clients,
                args.startup_timeout,
            )
            wait_for_log_count(
                base,
                compose_file,
                args.project,
                "event=tun_lease_assigned",
                args.clients,
                args.startup_timeout,
            )
            lease_suffixes = sorted(CLIENTS)[: args.clients]
            leases = wait_for_client_leases(
                base,
                compose_file,
                args.project,
                lease_suffixes,
                args.startup_timeout,
            )
            summary["leases"] = leases

            statuses = [
                query_status(base, compose_file, args.project, service)
                for service in ["fps-server", *[f"fps-client-{suffix}" for suffix in lease_suffixes]]
            ]
            assert_no_secret_status_leak(statuses)
            summary["initial_status"] = {
                "server_carriers": session_counter(statuses[0], "carriers_current"),
                "server_authenticated": auth_counter(statuses[0], "authenticated"),
            }

            phase_duration = max(1, args.duration)
            summary["mixed_client_to_server"] = run_sustained_mixed_traffic(
                base,
                compose_file,
                args.project,
                phase_duration,
                args.bandwidth,
                args.length,
            )
            summary["server_to_client_udp"] = run_server_to_client_routing(
                base,
                compose_file,
                args.project,
                leases,
                max(1, min(phase_duration, 10)),
                args.bandwidth,
                args.length,
            )
            summary["spoof_drop_liveness"] = run_spoof_drop_liveness(
                base,
                compose_file,
                args.project,
                args.startup_timeout,
                max(1, min(phase_duration, 10)),
                args.bandwidth,
                args.length,
            )
            summary["carrier_loss_recovery"] = run_carrier_loss_recovery(
                base,
                compose_file,
                args.project,
                leases,
                services,
                args.clients + 1,
                args.startup_timeout,
                max(1, min(phase_duration, 10)),
                args.bandwidth,
                args.length,
            )
            if args.with_socks_overlay:
                wait_for_socks(base, compose_file, args.project, args.startup_timeout)
                summary["socks_overlay"] = run_socks_probe(base, compose_file, args.project)
            if args.stress_backpressure:
                summary["backpressure"] = run_backpressure_stress(
                    base,
                    compose_file,
                    args.project,
                    max(3, min(phase_duration, 10)),
                    args.stress_bandwidth,
                    max(args.length, 1200),
                    args.startup_timeout,
                    args.require_backpressure_event,
                )

            running_after = compose(
                base, compose_file, args.project, ["ps", "--services", "--status", "running"]
            ).stdout.split()
            missing = sorted(set(services) - set(running_after))
            if missing:
                raise RuntimeError(f"services exited during soak: {missing}")
            summary["services_running_after_soak"] = sorted(running_after)

            final_statuses = [
                query_status(base, compose_file, args.project, service)
                for service in ["fps-server", *[f"fps-client-{suffix}" for suffix in lease_suffixes]]
            ]
            assert_no_secret_status_leak(final_statuses)
            summary["final_status"] = {
                "server": final_statuses[0]["sessions"],
                "tun": final_statuses[0]["tun"],
            }

            print(json.dumps(summary, indent=2, sort_keys=True))
        finally:
            try:
                logs = collect_logs(base, compose_file, args.project)
            except Exception:
                pass
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
        raise SystemExit(1)
