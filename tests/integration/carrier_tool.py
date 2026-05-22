#!/usr/bin/env python3

import argparse
import socket
import ssl
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from fps_https_harness import free_port, start_process, terminate, wait_for_output_line


def carrier_command(carrier):
    if carrier.endswith(".py"):
        return [sys.executable, carrier]
    return [carrier]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--carrier", required=True)
    args = parser.parse_args()

    origin = None
    with tempfile.TemporaryDirectory() as tmpdir_str:
        tmpdir = Path(tmpdir_str)
        cert = tmpdir / "carrier-cert.pem"
        key = tmpdir / "carrier-key.pem"
        ready = tmpdir / "client.ready"
        port = free_port()
        origin = start_process(
            [
                *carrier_command(args.carrier),
                "origin",
                "--listen",
                f"127.0.0.1:{port}",
                "--cert",
                str(cert),
                "--key",
                str(key),
                "--generate-self-signed",
                "--path",
                "/fps-carrier-test",
            ]
        )
        try:
            wait_for_output_line(origin, "carrier origin")
            context = ssl._create_unverified_context()
            with socket.create_connection(("127.0.0.1", port), timeout=5.0) as raw:
                with context.wrap_socket(raw, server_hostname="localhost") as sock:
                    sock.settimeout(5.0)
                    sock.sendall(
                        b"GET / HTTP/1.1\r\n"
                        b"Host: localhost\r\n"
                        b"Connection: close\r\n"
                        b"\r\n"
                    )
                    response = b""
                    while True:
                        chunk = sock.recv(4096)
                        if not chunk:
                            break
                        response += chunk
            if b"HTTP/1.1 200" not in response or b"fps_carrier echo origin" not in response:
                raise RuntimeError(f"unexpected carrier HTTPS response:\n{response!r}")

            completed = subprocess.run(
                [
                    *carrier_command(args.carrier),
                    "client",
                    "--connect",
                    f"127.0.0.1:{port}",
                    "--path",
                    "/fps-carrier-test",
                    "--frames",
                    "4",
                    "--client-bps",
                    "2048",
                    "--frame-rate",
                    "8",
                    "--seed",
                    "17",
                    "--ready-file",
                    str(ready),
                    "--no-reconnect",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=10.0,
            )
            if completed.returncode != 0:
                raise RuntimeError(f"carrier client failed:\n{completed.stdout}")
            if "READY client" not in completed.stdout or "DONE frames=4" not in completed.stdout:
                raise RuntimeError(f"unexpected carrier client output:\n{completed.stdout}")
            if not ready.exists():
                raise RuntimeError("carrier client did not write ready file")
        finally:
            terminate(origin)
            time.sleep(0.05)

    return 0


if __name__ == "__main__":
    sys.exit(main())
