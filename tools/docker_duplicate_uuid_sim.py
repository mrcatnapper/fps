#!/usr/bin/env python3

import argparse
import copy
import json
import os
import shutil
import shlex
import sys
import tempfile
import textwrap
import time
from pathlib import Path

from docker_multi_client_sim import (
    CLIENTS,
    REQUIRED_SERVICES,
    SERVER_IP,
    collect_logs,
    ignore_private_artifacts,
    lease_ip_from_addr_output,
    wait_for_log_count,
    write_compose,
    write_config,
)
from fps_docker_common import (
    compose,
    compose_exec,
    docker,
    docker_base,
    generate_server_keypair,
    repo_root,
    wait_for_services,
    write_json,
)


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
        if len(leases) == len(suffixes):
            return leases
        time.sleep(0.5)
    raise RuntimeError(f"clients did not receive leases: {last}")


def query_server_status(base: list[str], compose_file: Path, project: str) -> dict:
    result = compose_exec(
        base,
        compose_file,
        project,
        "fps-server",
        ["fps_server", "--status", "--config", "/etc/fps/server.json"],
    )
    return json.loads(result.stdout)


def start_udp_listener(
    base: list[str],
    compose_file: Path,
    project: str,
    service: str,
    bind_ip: str,
    port: int,
    marker_path: str,
):
    code = textwrap.dedent(
        f"""
        import pathlib
        import socket

        marker = pathlib.Path({marker_path!r})
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(6.0)
        sock.bind(({bind_ip!r}, {port}))
        try:
            data, _addr = sock.recvfrom(2048)
            marker.write_bytes(data)
        except TimeoutError:
            pass
        finally:
            sock.close()
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
            f"python3 -c {shlex.quote(code)} >/tmp/fps-udp-listener.log 2>&1 &",
        ],
    )
    time.sleep(0.5)


def send_udp_from_server(
    base: list[str],
    compose_file: Path,
    project: str,
    target_ip: str,
    port: int,
    payload: bytes,
):
    code = (
        "import socket;"
        "sock=socket.socket(socket.AF_INET, socket.SOCK_DGRAM);"
        f"sock.sendto({payload!r}, ({target_ip!r}, {port}));"
        "sock.close()"
    )
    compose_exec(base, compose_file, project, "fps-server", ["python3", "-c", code])


def container_file_exists(
    base: list[str],
    compose_file: Path,
    project: str,
    service: str,
    path: str,
) -> bool:
    result = compose_exec(
        base,
        compose_file,
        project,
        service,
        ["test", "-s", path],
        check=False,
    )
    return result.returncode == 0


def wait_for_container_file(
    base: list[str],
    compose_file: Path,
    project: str,
    service: str,
    path: str,
    timeout: float,
) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if container_file_exists(base, compose_file, project, service, path):
            return True
        time.sleep(0.25)
    return False


def main():
    parser = argparse.ArgumentParser(
        description="Run a Docker FPS duplicate-UUID replace_old simulation."
    )
    parser.add_argument("--image", default=os.environ.get("FPS_DOCKER_IMAGE", "fps:local"))
    parser.add_argument("--build", action="store_true", help="Build the image before running.")
    parser.add_argument("--sudo", action="store_true", help="Use sudo -n docker.")
    parser.add_argument("--project", default=f"fps-duplicate-uuid-{os.getpid()}")
    parser.add_argument("--mtu", type=int, default=1280)
    parser.add_argument("--carrier-bps", type=int, default=262144)
    parser.add_argument("--log-level", default="debug")
    parser.add_argument("--startup-timeout", type=float, default=60.0)
    parser.add_argument("--keep-artifacts", action="store_true")
    args = parser.parse_args()

    if args.mtu <= 0:
        raise ValueError("--mtu must be positive")
    if args.carrier_bps <= 0:
        raise ValueError("--carrier-bps must be positive")

    base = docker_base(args.sudo)
    root = repo_root()
    if args.build:
        docker(base, ["build", "-t", args.image, str(root)])

    original_clients = copy.deepcopy(CLIENTS)
    CLIENTS["b"]["uuid"] = CLIENTS["a"]["uuid"]
    try:
        with tempfile.TemporaryDirectory(prefix="fps-docker-duplicate-uuid-") as tmp:
            work = Path(tmp)
            config_dir = work / "config"
            config_dir.mkdir()
            server_keys = generate_server_keypair(base, args.image)
            allowed_uuids = sorted({data["uuid"] for data in CLIENTS.values()})
            profile_id = "docker-duplicate-uuid-v5"
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
            server_config = json.loads((config_dir / "server.json").read_text(encoding="utf-8"))
            server_config["ops"] = {"status_socket": "/run/fps/server.status"}
            write_json(config_dir / "server.json", server_config)
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
                first_services = [
                    "fps-carrier-origin",
                    "fps-server",
                    "fps-client-a",
                    "fps-carrier-client-a",
                ]
                compose(base, compose_file, args.project, ["up", "-d", *first_services])
                wait_for_services(base, compose_file, args.project, first_services, args.startup_timeout)
                wait_for_log_count(
                    base,
                    compose_file,
                    args.project,
                    "event=carrier_registered",
                    1,
                    args.startup_timeout,
                )
                first_lease = wait_for_client_leases(
                    base, compose_file, args.project, ["a"], args.startup_timeout
                )["a"]

                second_services = ["fps-client-b", "fps-carrier-client-b"]
                compose(base, compose_file, args.project, ["up", "-d", *second_services])
                wait_for_services(base, compose_file, args.project, REQUIRED_SERVICES, args.startup_timeout)
                wait_for_log_count(
                    base,
                    compose_file,
                    args.project,
                    "event=duplicate_client_replaced",
                    1,
                    args.startup_timeout,
                )
                compose(base, compose_file, args.project, ["stop", "-t", "5", "fps-carrier-client-a"])

                leases = wait_for_client_leases(
                    base, compose_file, args.project, ["a", "b"], args.startup_timeout
                )
                if leases["a"] != leases["b"] or leases["a"] != first_lease:
                    raise RuntimeError(f"duplicate clients did not share the same lease: {leases}")
                summary["lease"] = leases["b"]

                old_marker = "/tmp/fps-duplicate-old-received"
                start_udp_listener(
                    base,
                    compose_file,
                    args.project,
                    "fps-client-a",
                    leases["a"],
                    5610,
                    old_marker,
                )
                send_udp_from_server(
                    base, compose_file, args.project, leases["a"], 5610, b"old-instance-check"
                )
                time.sleep(2.0)
                if container_file_exists(
                    base, compose_file, args.project, "fps-client-a", old_marker
                ):
                    raise RuntimeError("old duplicate client received server-to-client traffic")
                summary["old_client_server_to_client_blocked"] = True

                new_marker = "/tmp/fps-duplicate-new-received"
                start_udp_listener(
                    base,
                    compose_file,
                    args.project,
                    "fps-client-b",
                    leases["b"],
                    5611,
                    new_marker,
                )
                send_udp_from_server(
                    base, compose_file, args.project, leases["b"], 5611, b"new-instance-check"
                )
                if not wait_for_container_file(
                    base,
                    compose_file,
                    args.project,
                    "fps-client-b",
                    new_marker,
                    args.startup_timeout,
                ):
                    raise RuntimeError("new duplicate client did not receive server-to-client traffic")
                summary["new_client_server_to_client_ok"] = True

                status = query_server_status(base, compose_file, args.project)
                replacements = status["sessions"].get("duplicate_client_replacements", 0)
                if replacements < 1:
                    raise RuntimeError(f"status did not report duplicate replacement: {status!r}")
                status_text = json.dumps(status)
                forbidden = [
                    CLIENTS["a"]["uuid"],
                    "allowed_client_uuids",
                    "server_private_key",
                    "ClientID",
                    "client_instance_id",
                ]
                if any(item in status_text for item in forbidden):
                    raise RuntimeError(f"status leaked sensitive duplicate metadata: {status!r}")
                summary["duplicate_client_replacements"] = replacements

                logs = collect_logs(base, compose_file, args.project)
                if "event=duplicate_client_replaced" not in logs:
                    raise RuntimeError("server logs did not contain duplicate replacement event")
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
    finally:
        CLIENTS.clear()
        CLIENTS.update(original_clients)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
