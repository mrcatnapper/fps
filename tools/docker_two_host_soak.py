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

from docker_multi_client_sim import CLIENTS, LEASE_POOL, SERVER_IP, SPOOF_IP, lease_ip_from_addr_output, write_config
from fps_docker_common import (
    compose,
    compose_exec,
    copy_tree_to_remote,
    docker,
    docker_base,
    ensure_safe_remote_work_dir,
    generate_server_keypair,
    remote_compose,
    remote_compose_exec,
    repo_root,
    run,
    ssh,
    transfer_docker_image,
    wait_for_services,
    write_json,
)


REMOTE_SERVICES = ["fps-carrier-origin", "fps-server"]
HTTP_PORT = 18081
BAD_LOG_PATTERNS = [
    "event=classified_record_encode_error",
    "event=classified_record_error",
    "event=envelope_encode_error",
    "event=shaper_snapshot_send_failed",
    "stage=classified_record error=oversized_payload",
]


def default_image() -> str:
    git = run(["git", "rev-parse", "--short", "HEAD"], check=False)
    if git is not None and git.returncode == 0:
        return f"fps:soak-{git.stdout.strip()}"
    return "fps:local"


def resolve_remote_connect_host(remote: str, explicit: str | None) -> str:
    if explicit:
        return explicit
    result = ssh(remote, "hostname -I | awk '{print $1}'", check=False)
    candidate = result.stdout.strip().splitlines()[0] if result.returncode == 0 and result.stdout.strip() else ""
    return candidate or remote


def selected_client_suffixes(count: int) -> list[str]:
    return sorted(CLIENTS)[:count]


def local_services(client_suffixes: list[str], carriers_per_client: int) -> list[str]:
    services = []
    for suffix in client_suffixes:
        services.append(f"fps-client-{suffix}")
        services.extend(f"fps-carrier-client-{suffix}{index}" for index in range(1, carriers_per_client + 1))
    return services


def patch_runtime_config(path: Path, *, status_socket: str, enable_shaper: bool, profile_id: str):
    config = json.loads(path.read_text(encoding="utf-8"))
    config["ops"] = {"status_socket": status_socket}
    if enable_shaper:
        config["shaper"] = {
            "enabled": True,
            "profile_id": profile_id,
            "record_size_cdf_c2s": [[4096, 1.0]],
            "record_size_cdf_s2c": [[4096, 1.0]],
            "inter_record_delay_us_cdf_c2s": [[5000, 1.0]],
            "inter_record_delay_us_cdf_s2c": [[5000, 1.0]],
            "covert_ratio_max": 1.0,
            "burst_records_max": 64,
            "jitter_ms": {"min": 0, "max": 0},
            "adaptive": {
                "enabled": True,
                "min_records": 16,
                "min_observation_ms": 2000,
                "decay": 0.98,
                "snapshot_interval_ms": 30000,
            },
            "deterministic_seed": 7,
        }
    write_json(path, config)


def write_remote_compose(path: Path, *, image: str, log_level: str, mtu: int, remote_port: int):
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
                ports:
                  - "{remote_port}:8443/tcp"
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
        ).strip()
        + "\n",
        encoding="utf-8",
    )


def write_local_compose(
    path: Path,
    *,
    image: str,
    log_level: str,
    mtu: int,
    client_suffixes: list[str],
    carriers_per_client: int,
    carrier_bps: int,
):
    rendered = []
    for suffix in client_suffixes:
        data = CLIENTS[suffix]
        rendered.append(
            f"""
              fps-client-{suffix}:
                image: {image}
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
        for index in range(1, carriers_per_client + 1):
            rendered.append(
                f"""
                  fps-carrier-client-{suffix}{index}:
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
                      - "{data["seed"]}{index}"
                    restart: unless-stopped
                    security_opt:
                      - no-new-privileges:true
                """
            )

    path.write_text(
        "services:\n"
        + "\n".join(textwrap.indent(textwrap.dedent(item).strip(), "  ") for item in rendered)
        + "\n",
        encoding="utf-8",
    )


def build_image(base: list[str], root: Path, dockerfile: str, image: str):
    docker(base, ["build", "-f", str(root / dockerfile), "-t", image, str(root)])


def query_local_status(base: list[str], compose_file: Path, project: str, service: str) -> dict:
    binary = "fps_server" if service == "fps-server" else "fps_client"
    config = "/etc/fps/server.json" if service == "fps-server" else "/etc/fps/client.json"
    result = compose_exec(base, compose_file, project, service, [binary, "--status", "--config", config])
    return json.loads(result.stdout)


def query_remote_status(remote: str, remote_dir: str, project: str, remote_docker: str) -> dict:
    result = remote_compose_exec(
        remote,
        remote_dir,
        project,
        "fps-server",
        ["fps_server", "--status", "--config", "/etc/fps/server.json"],
        remote_docker=remote_docker,
    )
    return json.loads(result.stdout)


def wait_until(predicate, *, timeout: float, description: str):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(0.5)
    raise RuntimeError(f"timed out waiting for {description}: {last!r}")


def wait_for_local_client_leases(
    base: list[str],
    compose_file: Path,
    project: str,
    suffixes: list[str],
    timeout: float,
) -> dict[str, str]:
    last = {}

    def probe():
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
        return None

    result = wait_until(probe, timeout=timeout, description=f"leases for {suffixes}")
    if not result:
        raise RuntimeError(f"clients did not receive distinct leases: {last}")
    return result


def wait_for_carrier_counts(
    base: list[str],
    local_compose: Path,
    project: str,
    remote: str,
    remote_dir: str,
    remote_docker: str,
    suffixes: list[str],
    carriers_per_client: int,
    timeout: float,
):
    expected_total = len(suffixes) * carriers_per_client

    def probe():
        server = query_remote_status(remote, remote_dir, project, remote_docker)
        if int(server.get("sessions", {}).get("carriers_current", 0) or 0) < expected_total:
            return None
        for suffix in suffixes:
            client = query_local_status(base, local_compose, project, f"fps-client-{suffix}")
            if int(client.get("sessions", {}).get("carriers_current", 0) or 0) < carriers_per_client:
                return None
        return server

    return wait_until(probe, timeout=timeout, description="all carrier sessions to authenticate")


def start_remote_udp_echo(remote: str, remote_dir: str, project: str, remote_docker: str, port: int, duration: int):
    marker = f"/tmp/fps-two-host-udp-server-{port}.json"
    code = textwrap.dedent(
        f"""
        import json
        import pathlib
        import socket
        import time

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(({SERVER_IP!r}, {port}))
        sock.settimeout(0.5)
        deadline = time.monotonic() + {duration + 30}
        received = 0
        sent = 0
        errors = 0
        def write_marker():
            pathlib.Path({marker!r}).write_text(json.dumps({{"received": received, "sent": sent, "errors": errors}}), encoding="utf-8")

        next_marker = time.monotonic()
        while time.monotonic() < deadline:
            try:
                data, addr = sock.recvfrom(65535)
                received += 1
                sock.sendto(data, addr)
                sent += 1
            except socket.timeout:
                pass
            except Exception:
                errors += 1
            if time.monotonic() >= next_marker:
                write_marker()
                next_marker = time.monotonic() + 1.0
        write_marker()
        """
    )
    remote_compose_exec(
        remote,
        remote_dir,
        project,
        "fps-server",
        ["sh", "-lc", f"rm -f {shlex.quote(marker)}; python3 -c {shlex.quote(code)} >/tmp/fps-two-host-udp-server-{port}.log 2>&1 &"],
        remote_docker=remote_docker,
    )
    return marker


def start_local_udp_client(
    base: list[str],
    compose_file: Path,
    project: str,
    suffix: str,
    lease_ip: str,
    port: int,
    duration: int,
    pps: float,
    payload_bytes: int,
):
    marker = f"/tmp/fps-two-host-udp-client-{suffix}.json"
    code = textwrap.dedent(
        f"""
        import json
        import pathlib
        import socket
        import time

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(({lease_ip!r}, 0))
        interval = 1.0 / {pps!r}
        deadline = time.monotonic() + {duration}
        seq = 0
        stats = {{"sent": 0, "received": 0, "duplicate_or_late": 0, "bad": 0}}
        pending = set()
        next_send = time.monotonic()
        payload_len = max(0, {payload_bytes} - 8)

        def payload_for(number):
            return number.to_bytes(8, "big") + bytes([(number + {ord(suffix)}) % 251]) * payload_len

        def drain(until_time):
            while True:
                remaining = until_time - time.monotonic()
                if remaining <= 0:
                    return
                sock.settimeout(min(0.05, remaining))
                try:
                    data, _ = sock.recvfrom(65535)
                except socket.timeout:
                    remaining = until_time - time.monotonic()
                    if remaining > 0:
                        time.sleep(remaining)
                    return
                if len(data) < 8:
                    stats["bad"] += 1
                    continue
                number = int.from_bytes(data[:8], "big")
                if data != payload_for(number):
                    stats["bad"] += 1
                elif number in pending:
                    pending.remove(number)
                    stats["received"] += 1
                else:
                    stats["duplicate_or_late"] += 1

        while time.monotonic() < deadline:
            seq += 1
            payload = payload_for(seq)
            sock.sendto(payload, ({SERVER_IP!r}, {port}))
            pending.add(seq)
            stats["sent"] += 1
            next_send += interval
            drain(next_send)
        drain(time.monotonic() + 2.0)
        lost = len(pending)
        pathlib.Path({marker!r}).write_text(
            json.dumps({{**stats, "lost": lost, "loss_percent": (lost / stats["sent"] * 100.0) if stats["sent"] else 0.0}}),
            encoding="utf-8",
        )
        """
    )
    compose_exec(
        base,
        compose_file,
        project,
        f"fps-client-{suffix}",
        ["sh", "-lc", f"rm -f {shlex.quote(marker)}; python3 -c {shlex.quote(code)} >/tmp/fps-two-host-udp-client-{suffix}.log 2>&1 &"],
    )
    return marker


def start_remote_http_server(remote: str, remote_dir: str, project: str, remote_docker: str):
    remote_compose_exec(
        remote,
        remote_dir,
        project,
        "fps-server",
        [
            "sh",
            "-lc",
            "if [ -f /tmp/fps-two-host-http.pid ]; then kill \"$(cat /tmp/fps-two-host-http.pid)\" >/dev/null 2>&1 || true; fi; "
            f"python3 -m http.server {HTTP_PORT} --bind {SERVER_IP} >/tmp/fps-two-host-http.log 2>&1 & "
            "echo $! >/tmp/fps-two-host-http.pid",
        ],
        remote_docker=remote_docker,
    )


def start_local_http_probe(
    base: list[str],
    compose_file: Path,
    project: str,
    suffix: str,
    duration: int,
):
    marker = f"/tmp/fps-two-host-http-client-{suffix}.json"
    code = textwrap.dedent(
        f"""
        import json
        import pathlib
        import socket
        import time

        deadline = time.monotonic() + {duration}
        ok = 0
        fail = 0
        last_error = ""
        while time.monotonic() < deadline:
            try:
                sock = socket.create_connection(({SERVER_IP!r}, {HTTP_PORT}), 3.0)
                sock.sendall(b"GET / HTTP/1.1\\r\\nHost: fps-two-host\\r\\nConnection: close\\r\\n\\r\\n")
                data = b""
                while True:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                sock.close()
                if b"200 OK" in data:
                    ok += 1
                else:
                    fail += 1
                    last_error = data[:120].decode("latin1", "replace")
            except Exception as error:
                fail += 1
                last_error = str(error)
            time.sleep(0.5)
        pathlib.Path({marker!r}).write_text(json.dumps({{"ok": ok, "fail": fail, "last_error": last_error}}), encoding="utf-8")
        """
    )
    compose_exec(
        base,
        compose_file,
        project,
        f"fps-client-{suffix}",
        ["sh", "-lc", f"rm -f {shlex.quote(marker)}; python3 -c {shlex.quote(code)} >/tmp/fps-two-host-http-client-{suffix}.log 2>&1 &"],
    )
    return marker


def wait_for_local_json_marker(base: list[str], compose_file: Path, project: str, service: str, marker: str, timeout: float) -> dict:
    last = None

    def probe():
        nonlocal last
        result = compose_exec(base, compose_file, project, service, ["sh", "-lc", f"cat {shlex.quote(marker)}"], check=False)
        if result.returncode != 0:
            last = result.stdout
            return None
        try:
            return json.loads(result.stdout)
        except json.JSONDecodeError as error:
            last = str(error)
            return None

    result = wait_until(probe, timeout=timeout, description=f"JSON marker {service}:{marker}")
    if not result:
        raise RuntimeError(f"marker did not appear: {service}:{marker}: {last}")
    return result


def wait_for_remote_json_marker(remote: str, remote_dir: str, project: str, remote_docker: str, service: str, marker: str, timeout: float) -> dict:
    last = None

    def probe():
        nonlocal last
        result = remote_compose_exec(
            remote,
            remote_dir,
            project,
            service,
            ["sh", "-lc", f"cat {shlex.quote(marker)}"],
            remote_docker=remote_docker,
            check=False,
        )
        if result.returncode != 0:
            last = result.stdout
            return None
        try:
            return json.loads(result.stdout)
        except json.JSONDecodeError as error:
            last = str(error)
            return None

    result = wait_until(probe, timeout=timeout, description=f"remote JSON marker {service}:{marker}")
    if not result:
        raise RuntimeError(f"remote marker did not appear: {service}:{marker}: {last}")
    return result


def send_spoofed_udp(base: list[str], compose_file: Path, project: str, suffix: str):
    service = f"fps-client-{suffix}"
    compose_exec(base, compose_file, project, service, ["ip", "addr", "add", f"{SPOOF_IP}/32", "dev", "fpsc0"], check=False)
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
                f"s.bind(({SPOOF_IP!r}, 0));"
                f"s.sendto(b'fps-two-host-spoof', ({SERVER_IP!r}, 9));"
                "s.close()"
            ),
        ],
    )


def restart_carriers(base: list[str], compose_file: Path, project: str, services: list[str]):
    compose(base, compose_file, project, ["stop", "-t", "5", *services])
    time.sleep(1.0)
    compose(base, compose_file, project, ["start", *services])


def planned_restarts(client_suffixes: list[str], carriers_per_client: int, duration: int) -> list[tuple[float, list[str]]]:
    if duration < 30:
        return []
    if carriers_per_client >= 2 and len(client_suffixes) >= 2:
        return [
            (duration * 0.25, [f"fps-carrier-client-{client_suffixes[0]}1"]),
            (duration * 0.45, [f"fps-carrier-client-{client_suffixes[1]}2"]),
            (duration * 0.65, [f"fps-carrier-client-{client_suffixes[0]}2", f"fps-carrier-client-{client_suffixes[1]}1"]),
        ]
    return [(duration * 0.5, [f"fps-carrier-client-{client_suffixes[0]}1"])]


def collect_local_logs(base: list[str], compose_file: Path, project: str, services: list[str]) -> str:
    return compose(base, compose_file, project, ["logs", "--no-color", *services], check=False).stdout


def collect_remote_logs(remote: str, remote_dir: str, project: str, remote_docker: str) -> str:
    return remote_compose(remote, remote_dir, project, ["logs", "--no-color", *REMOTE_SERVICES], remote_docker=remote_docker, check=False).stdout


def collect_local_tmp_logs(base: list[str], compose_file: Path, project: str, services: list[str]) -> str:
    chunks = []
    command = "for f in /tmp/fps-two-host*.log; do [ -e \"$f\" ] || continue; echo ==== $f; cat \"$f\"; done"
    for service in services:
        result = compose_exec(base, compose_file, project, service, ["sh", "-lc", command], check=False)
        if result.stdout:
            chunks.append(f"### {service}\n{result.stdout}")
    return "\n".join(chunks)


def collect_remote_tmp_logs(remote: str, remote_dir: str, project: str, remote_docker: str) -> str:
    chunks = []
    command = "for f in /tmp/fps-two-host*.log; do [ -e \"$f\" ] || continue; echo ==== $f; cat \"$f\"; done"
    for service in REMOTE_SERVICES:
        result = remote_compose_exec(
            remote,
            remote_dir,
            project,
            service,
            ["sh", "-lc", command],
            remote_docker=remote_docker,
            check=False,
        )
        if result.stdout:
            chunks.append(f"### {service}\n{result.stdout}")
    return "\n".join(chunks)


def count_bad_logs(*logs: str) -> dict[str, int]:
    combined = "\n".join(logs)
    return {pattern: combined.count(pattern) for pattern in BAD_LOG_PATTERNS}


def assert_no_secret_status_leak(statuses: list[dict]):
    text = json.dumps(statuses, sort_keys=True)
    forbidden = ["client_uuid", "allowed_client_uuids", "server_private_key", "ClientID"]
    leaked = [item for item in forbidden if item in text]
    if leaked:
        raise RuntimeError(f"status leaked sensitive fields: {leaked}")


def persist_artifacts(
    project: str,
    work: Path,
    summary: dict,
    local_logs: str,
    remote_logs: str,
    local_tmp_logs: str,
    remote_tmp_logs: str,
    local_status: dict,
    remote_status: dict,
) -> Path:
    out = Path.cwd() / "captures" / project
    if out.exists():
        shutil.rmtree(out)
    (out / "local").mkdir(parents=True)
    (out / "remote").mkdir(parents=True)
    shutil.copy2(work / "local" / "compose.yml", out / "local" / "compose.yml")
    shutil.copy2(work / "remote" / "compose.yml", out / "remote" / "compose.yml")
    write_json(out / "summary.json", summary)
    (out / "local.logs").write_text(local_logs, encoding="utf-8")
    (out / "remote.logs").write_text(remote_logs, encoding="utf-8")
    (out / "local-tmp.logs").write_text(local_tmp_logs, encoding="utf-8")
    (out / "remote-tmp.logs").write_text(remote_tmp_logs, encoding="utf-8")
    write_json(out / "local-status.json", local_status)
    write_json(out / "remote-status.json", remote_status)
    return out


def main():
    parser = argparse.ArgumentParser(description="Run a split-host Docker FPS soak with local clients and a remote server.")
    parser.add_argument("--remote", default="fpshop", help="SSH target for the remote server/origin stack.")
    parser.add_argument(
        "--remote-connect-host",
        default=None,
        help="Host/IP local Docker clients use to reach the remote FPS server. Defaults to the first remote hostname -I address, then --remote.",
    )
    parser.add_argument("--remote-port", type=int, default=443)
    parser.add_argument("--remote-dir", default=None, help="Remote temporary directory. Defaults to /tmp/<project>.")
    parser.add_argument("--remote-docker", default="docker", help="Remote Docker command, e.g. 'docker' or 'sudo -n docker'.")
    parser.add_argument("--image", default=None)
    parser.add_argument("--dockerfile", default="Dockerfile.alpine")
    parser.add_argument("--build-local", action="store_true", help="Build the runtime image locally before running.")
    parser.add_argument("--transfer-image", action="store_true", help="Transfer the local image to the remote host before running.")
    parser.add_argument("--sudo", action="store_true", help="Use sudo -n docker locally.")
    parser.add_argument("--project", default=f"fps-two-host-soak-{os.getpid()}")
    parser.add_argument("--duration", type=int, default=300)
    parser.add_argument("--clients", type=int, choices=(1, 2), default=2)
    parser.add_argument("--carriers-per-client", type=int, default=2)
    parser.add_argument("--bandwidth", default="500K", help="Compatibility label for summaries; UDP loop uses --udp-pps.")
    parser.add_argument("--length", type=int, default=512)
    parser.add_argument("--udp-pps", type=float, default=8.0)
    parser.add_argument("--max-loss-percent", type=float, default=1.0)
    parser.add_argument("--mtu", type=int, default=1280)
    parser.add_argument("--carrier-bps", type=int, default=262144)
    parser.add_argument("--log-level", default="debug")
    parser.add_argument("--startup-timeout", type=float, default=90.0)
    parser.add_argument("--no-shaper", action="store_true")
    parser.add_argument("--keep-artifacts", action="store_true")
    args = parser.parse_args()

    if args.duration <= 0:
        raise ValueError("--duration must be positive")
    if args.carriers_per_client <= 0:
        raise ValueError("--carriers-per-client must be positive")
    if args.length < 8:
        raise ValueError("--length must be at least 8")
    if args.udp_pps <= 0:
        raise ValueError("--udp-pps must be positive")
    if args.max_loss_percent < 0:
        raise ValueError("--max-loss-percent must not be negative")

    base = docker_base(args.sudo)
    root = repo_root()
    image = args.image or default_image()
    remote_connect = resolve_remote_connect_host(args.remote, args.remote_connect_host)
    remote_dir = args.remote_dir or f"/tmp/{args.project}"
    client_suffixes = selected_client_suffixes(args.clients)
    local_service_names = local_services(client_suffixes, args.carriers_per_client)
    transferred = False

    if args.build_local:
        build_image(base, root, args.dockerfile, image)
        args.transfer_image = True
    if args.transfer_image:
        transfer_docker_image(base, args.remote, image, remote_docker=args.remote_docker)
        transferred = True

    with tempfile.TemporaryDirectory(prefix="fps-two-host-soak-") as tmp:
        work = Path(tmp)
        local_dir = work / "local"
        remote_local_dir = work / "remote"
        (local_dir / "config").mkdir(parents=True)
        (remote_local_dir / "config").mkdir(parents=True)

        server_keys = generate_server_keypair(base, image)
        allowed_uuids = [CLIENTS[suffix]["uuid"] for suffix in client_suffixes]
        profile_id = "docker-two-host-soak-v5"

        write_config(
            remote_local_dir / "config" / "server.json",
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
        patch_runtime_config(
            remote_local_dir / "config" / "server.json",
            status_socket="/run/fps/server.status",
            enable_shaper=not args.no_shaper,
            profile_id=profile_id,
        )

        for suffix in client_suffixes:
            write_config(
                local_dir / "config" / f"client-{suffix}.json",
                role="client",
                peer=f"{remote_connect}:{args.remote_port}",
                listen_port=CLIENTS[suffix]["listen"],
                tun_name="fpsc0",
                mtu=args.mtu,
                profile_id=profile_id,
                client_uuid=CLIENTS[suffix]["uuid"],
                allowed_client_uuids=[],
                server_private_key_base64=server_keys["server_private_key_base64"],
                server_public_key_base64=server_keys["server_public_key_base64"],
            )
            patch_runtime_config(
                local_dir / "config" / f"client-{suffix}.json",
                status_socket="/run/fps/client.status",
                enable_shaper=not args.no_shaper,
                profile_id=profile_id,
            )

        write_remote_compose(remote_local_dir / "compose.yml", image=image, log_level=args.log_level, mtu=args.mtu, remote_port=args.remote_port)
        write_local_compose(
            local_dir / "compose.yml",
            image=image,
            log_level=args.log_level,
            mtu=args.mtu,
            client_suffixes=client_suffixes,
            carriers_per_client=args.carriers_per_client,
            carrier_bps=args.carrier_bps,
        )

        summary = {
            "project": args.project,
            "image": image,
            "remote": args.remote,
            "remote_connect": remote_connect,
            "remote_port": args.remote_port,
            "duration": args.duration,
            "clients": client_suffixes,
            "carriers_per_client": args.carriers_per_client,
            "shaper_enabled": not args.no_shaper,
            "image_transferred": transferred,
            "bandwidth_label": args.bandwidth,
            "udp_pps": args.udp_pps,
            "udp_payload_bytes": args.length,
        }
        local_logs = ""
        remote_logs = ""
        local_tmp_logs = ""
        remote_tmp_logs = ""
        local_status = {}
        remote_status = {}
        failed = True
        try:
            copy_tree_to_remote(remote_local_dir, args.remote, remote_dir)
            remote_compose(args.remote, remote_dir, args.project, ["up", "-d"], remote_docker=args.remote_docker)
            wait_until(
                lambda: set(remote_compose(args.remote, remote_dir, args.project, ["ps", "--services", "--status", "running"], remote_docker=args.remote_docker).stdout.split())
                >= set(REMOTE_SERVICES),
                timeout=args.startup_timeout,
                description="remote server services",
            )

            compose(base, local_dir / "compose.yml", args.project, ["up", "-d", *local_service_names])
            wait_for_services(base, local_dir / "compose.yml", args.project, local_service_names, args.startup_timeout)
            leases = wait_for_local_client_leases(base, local_dir / "compose.yml", args.project, client_suffixes, args.startup_timeout)
            summary["leases"] = leases
            wait_for_carrier_counts(
                base,
                local_dir / "compose.yml",
                args.project,
                args.remote,
                remote_dir,
                args.remote_docker,
                client_suffixes,
                args.carriers_per_client,
                args.startup_timeout,
            )

            start_remote_http_server(args.remote, remote_dir, args.project, args.remote_docker)
            udp_server_markers = {}
            udp_client_markers = {}
            http_client_markers = {}
            for index, suffix in enumerate(client_suffixes, start=1):
                port = 5800 + index
                udp_server_markers[suffix] = start_remote_udp_echo(args.remote, remote_dir, args.project, args.remote_docker, port, args.duration)
                udp_client_markers[suffix] = start_local_udp_client(
                    base,
                    local_dir / "compose.yml",
                    args.project,
                    suffix,
                    leases[suffix],
                    port,
                    args.duration,
                    args.udp_pps,
                    args.length,
                )
                http_client_markers[suffix] = start_local_http_probe(base, local_dir / "compose.yml", args.project, suffix, args.duration)

            restarts = []
            started_at = time.monotonic()
            for offset, services in planned_restarts(client_suffixes, args.carriers_per_client, args.duration):
                sleep_for = started_at + offset - time.monotonic()
                if sleep_for > 0:
                    time.sleep(sleep_for)
                restart_carriers(base, local_dir / "compose.yml", args.project, services)
                restarts.extend(services)
                wait_for_services(base, local_dir / "compose.yml", args.project, local_service_names, args.startup_timeout)
                wait_for_carrier_counts(
                    base,
                    local_dir / "compose.yml",
                    args.project,
                    args.remote,
                    remote_dir,
                    args.remote_docker,
                    client_suffixes,
                    args.carriers_per_client,
                    args.startup_timeout,
                )
            remaining = started_at + args.duration - time.monotonic()
            if remaining > 0:
                time.sleep(remaining)
            summary["carrier_restarts"] = restarts

            udp_summary = {}
            http_summary = {}
            for suffix in client_suffixes:
                udp_summary[suffix] = wait_for_local_json_marker(
                    base,
                    local_dir / "compose.yml",
                    args.project,
                    f"fps-client-{suffix}",
                    udp_client_markers[suffix],
                    args.startup_timeout,
                )
                server_udp = wait_for_remote_json_marker(
                    args.remote,
                    remote_dir,
                    args.project,
                    args.remote_docker,
                    "fps-server",
                    udp_server_markers[suffix],
                    args.startup_timeout,
                )
                udp_summary[suffix]["server"] = server_udp
                http_summary[suffix] = wait_for_local_json_marker(
                    base,
                    local_dir / "compose.yml",
                    args.project,
                    f"fps-client-{suffix}",
                    http_client_markers[suffix],
                    args.startup_timeout,
                )
            summary["udp"] = udp_summary
            summary["http"] = http_summary
            for suffix in client_suffixes:
                if udp_summary[suffix]["bad"] != 0 or udp_summary[suffix]["loss_percent"] > args.max_loss_percent:
                    raise RuntimeError(f"UDP probe for client {suffix} lost or corrupted packets: {udp_summary[suffix]!r}")
                if http_summary[suffix]["ok"] <= 0 or http_summary[suffix]["fail"] != 0:
                    raise RuntimeError(f"HTTP probe for client {suffix} failed: {http_summary[suffix]!r}")

            send_spoofed_udp(base, local_dir / "compose.yml", args.project, client_suffixes[0])
            wait_until(
                lambda: int(query_remote_status(args.remote, remote_dir, args.project, args.remote_docker).get("tun", {}).get("tun_tunnel_events", {}).get("ignored_spoofed_tun_source", 0) or 0),
                timeout=args.startup_timeout,
                description="remote spoof-drop counter",
            )

            running_local = set(compose(base, local_dir / "compose.yml", args.project, ["ps", "--services", "--status", "running"]).stdout.split())
            running_remote = set(remote_compose(args.remote, remote_dir, args.project, ["ps", "--services", "--status", "running"], remote_docker=args.remote_docker).stdout.split())
            local_missing = sorted(set(local_service_names) - running_local)
            remote_missing = sorted(set(REMOTE_SERVICES) - running_remote)
            if local_missing or remote_missing:
                raise RuntimeError(f"services exited: local={local_missing} remote={remote_missing}")
            summary["running_local"] = sorted(running_local)
            summary["running_remote"] = sorted(running_remote)

            remote_status = query_remote_status(args.remote, remote_dir, args.project, args.remote_docker)
            local_status = {
                suffix: query_local_status(base, local_dir / "compose.yml", args.project, f"fps-client-{suffix}")
                for suffix in client_suffixes
            }
            assert_no_secret_status_leak([remote_status, *local_status.values()])
            summary["server_status"] = {
                "sessions": remote_status.get("sessions", {}),
                "auth": remote_status.get("auth", {}),
                "classified_record": remote_status.get("classified_record", {}),
                "tun": remote_status.get("tun", {}),
                "shaper": remote_status.get("shaper", {}),
            }
            summary["client_status"] = {
                suffix: {
                    "sessions": status.get("sessions", {}),
                    "auth": status.get("auth", {}),
                    "classified_record": status.get("classified_record", {}),
                    "tun": status.get("tun", {}),
                    "shaper": status.get("shaper", {}),
                }
                for suffix, status in local_status.items()
            }

            local_logs = collect_local_logs(base, local_dir / "compose.yml", args.project, local_service_names)
            remote_logs = collect_remote_logs(args.remote, remote_dir, args.project, args.remote_docker)
            summary["bad_log_counts"] = count_bad_logs(local_logs, remote_logs)
            bad = {pattern: count for pattern, count in summary["bad_log_counts"].items() if count}
            if bad:
                raise RuntimeError(f"bad log patterns found: {bad}")

            failed = False
            print(json.dumps(summary, indent=2, sort_keys=True))
        finally:
            try:
                local_logs = local_logs or collect_local_logs(base, local_dir / "compose.yml", args.project, local_service_names)
            except Exception:
                pass
            try:
                remote_logs = remote_logs or collect_remote_logs(args.remote, remote_dir, args.project, args.remote_docker)
            except Exception:
                pass
            try:
                local_tmp_logs = collect_local_tmp_logs(base, local_dir / "compose.yml", args.project, local_service_names)
            except Exception:
                pass
            try:
                remote_tmp_logs = collect_remote_tmp_logs(args.remote, remote_dir, args.project, args.remote_docker)
            except Exception:
                pass
            if args.keep_artifacts or failed:
                artifact_dir = persist_artifacts(
                    args.project,
                    work,
                    summary,
                    local_logs,
                    remote_logs,
                    local_tmp_logs,
                    remote_tmp_logs,
                    local_status,
                    remote_status,
                )
                print(f"artifacts={artifact_dir}", file=sys.stderr)
            compose(base, local_dir / "compose.yml", args.project, ["down", "-v", "--remove-orphans"], check=False)
            remote_compose(args.remote, remote_dir, args.project, ["down", "-v", "--remove-orphans"], remote_docker=args.remote_docker, check=False)
            if not args.keep_artifacts:
                remote_dir = ensure_safe_remote_work_dir(remote_dir)
                ssh(args.remote, f"rm -rf {shlex.quote(remote_dir)}", check=False)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
