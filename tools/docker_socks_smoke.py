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
    wait_for_tun,
    write_config,
)


REQUIRED_SERVICES = [
    "fps-carrier-origin",
    "fps-server",
    "fps-dante-proxy",
    "fps-client",
    "fps-carrier-client",
    "socks-http-origin",
]


def write_compose(
    path: Path,
    image: str,
    proxy_image: str,
    log_level: str,
    mtu: int,
    carrier_bps: int,
):
    path.write_text(
        textwrap.dedent(
            f"""
            services:
              socks-http-origin:
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

              fps-dante-proxy:
                image: {proxy_image}
                depends_on:
                  - fps-server
                network_mode: "service:fps-server"
                restart: unless-stopped
                environment:
                  FPS_SOCKS_LISTEN_ADDRESS: 10.88.0.1
                  FPS_SOCKS_PORT: "1080"
                  FPS_SOCKS_ALLOWED_CIDR: 10.88.0.0/30
                  FPS_SOCKS_EXTERNAL_INTERFACE: eth0
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
                  - "5151"
                restart: unless-stopped
                security_opt:
                  - no-new-privileges:true
            """
        ).strip()
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


def wait_for_socks(base: list[str], compose_file: Path, project: str, timeout: float):
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        result = compose_exec(
            base,
            compose_file,
            project,
            "fps-client",
            [
                "python3",
                "-c",
                (
                    "import socket; "
                    "s=socket.create_connection(('10.88.0.1',1080),1); "
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


def run_socks_http_probe(base: list[str], compose_file: Path, project: str) -> str:
    code = r"""
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

sock = socket.create_connection(("10.88.0.1", 1080), 5)
sock.sendall(b"\x05\x01\x00")
if recvn(sock, 2) != b"\x05\x00":
    raise RuntimeError("SOCKS server rejected no-auth method")

host = b"socks-http-origin"
port = 18080
sock.sendall(b"\x05\x01\x00\x03" + bytes([len(host)]) + host + struct.pack("!H", port))
reply = recvn(sock, 4)
if reply[1] != 0:
    raise RuntimeError(f"SOCKS connect failed with status {reply[1]}")
if reply[3] == 1:
    recvn(sock, 4)
elif reply[3] == 3:
    recvn(sock, recvn(sock, 1)[0])
elif reply[3] == 4:
    recvn(sock, 16)
else:
    raise RuntimeError("invalid SOCKS bind address type")
recvn(sock, 2)

sock.sendall(
    b"GET / HTTP/1.1\r\n"
    b"Host: socks-http-origin\r\n"
    b"Connection: close\r\n\r\n"
)
body = b""
while True:
    chunk = sock.recv(4096)
    if not chunk:
        break
    body += chunk
if b"200 OK" not in body:
    raise RuntimeError(body[:200].decode("latin1", "replace"))
print("socks_http_ok")
"""
    completed = compose_exec(
        base,
        compose_file,
        project,
        "fps-client",
        ["python3", "-c", code],
    )
    return completed.stdout.strip()


def main():
    parser = argparse.ArgumentParser(
        description="Run the Docker Dante SOCKS5 overlay example over an FPS TUN."
    )
    parser.add_argument("--image", default=os.environ.get("FPS_DOCKER_IMAGE", "fps:local"))
    parser.add_argument(
        "--proxy-image",
        default=os.environ.get("FPS_DANTE_PROXY_IMAGE", "fps-dante-proxy:local"),
    )
    parser.add_argument("--build", action="store_true", help="Build the image before running.")
    parser.add_argument("--sudo", action="store_true", help="Use sudo -n docker.")
    parser.add_argument("--project", default=f"fps-dante-{os.getpid()}")
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
        docker(
            base,
            [
                "build",
                "-f",
                str(root / "examples/docker/proxy-dante/Dockerfile"),
                "--build-arg",
                f"FPS_BASE_IMAGE={args.image}",
                "-t",
                args.proxy_image,
                str(root),
            ],
        )

    with tempfile.TemporaryDirectory(prefix="fps-docker-dante-") as tmp:
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
            "docker-dante-v5",
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
            "docker-dante-v5",
            CLIENT_UUID,
            server_keys["server_private_key_base64"],
            server_keys["server_public_key_base64"],
        )
        compose_file = work / "compose.yml"
        write_compose(
            compose_file,
            args.image,
            args.proxy_image,
            args.log_level,
            args.mtu,
            args.carrier_bps,
        )

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
            wait_for_socks(base, compose_file, args.project, args.startup_timeout)
            summary["probe"] = run_socks_http_probe(base, compose_file, args.project)

            running_after = compose(
                base, compose_file, args.project, ["ps", "--services", "--status", "running"]
            ).stdout.split()
            missing = sorted(set(REQUIRED_SERVICES) - set(running_after))
            if missing:
                raise RuntimeError(f"services exited during SOCKS smoke: {missing}")
            summary["services_running_after_probe"] = sorted(running_after)
            print(json.dumps(summary, indent=2, sort_keys=True))
        finally:
            logs = compose(
                base,
                compose_file,
                args.project,
                ["logs", "--no-color", *REQUIRED_SERVICES],
                check=False,
            ).stdout
            (work / "compose.logs").write_text(logs, encoding="utf-8")
            if args.keep_artifacts:
                persistent = Path.cwd() / "captures" / args.project
                if persistent.exists():
                    shutil.rmtree(persistent)
                persistent.parent.mkdir(parents=True, exist_ok=True)
                shutil.copytree(work, persistent)
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
