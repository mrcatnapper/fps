#!/usr/bin/env python3

import argparse
import json
import os
import socket
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from fps_https_harness import free_port


def run_command(args):
    return subprocess.run(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def wait_for(predicate, timeout=5.0, interval=0.05):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(interval)
    return None


def query_status(binary, config_path, socket_path):
    completed = run_command(
        [
            binary,
            "--status",
            "--config",
            str(config_path),
            "--status-socket",
            str(socket_path),
        ]
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"--status returned {completed.returncode}, stderr={completed.stderr!r}"
        )
    if completed.stderr:
        raise RuntimeError(f"--status wrote stderr: {completed.stderr!r}")
    return json.loads(completed.stdout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps-server", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="fps-status-socket-") as temp_name:
        temp = Path(temp_name)
        listen_port = free_port()
        target_port = free_port()
        status_socket = temp / "fps.status"
        config_path = temp / "server.json"
        config_path.write_text(
            json.dumps(
                {
                    "network": {
                        "listen": f"127.0.0.1:{listen_port}",
                        "origin": f"127.0.0.1:{target_port}",
                    },
                    "logging": {"level": "off"},
                    "ops": {"status_socket": str(status_socket)},
                    "shaper": {
                        "enabled": True,
                        "profile_id": "status-smoke",
                        "record_size_cdf_c2s": [[4096, 1.0]],
                        "record_size_cdf_s2c": [[4096, 1.0]],
                        "inter_record_delay_us_cdf_c2s": [[1000, 1.0]],
                        "inter_record_delay_us_cdf_s2c": [[1000, 1.0]],
                        "covert_ratio_max": 1.0,
                        "burst_records_max": 2,
                    },
                }
            ),
            encoding="utf-8",
        )

        process = subprocess.Popen(
            [args.fps_server, "--config", str(config_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            if wait_for(lambda: status_socket.exists()) is None:
                stderr = process.stderr.read() if process.poll() is not None else ""
                raise RuntimeError(f"status socket did not appear, stderr={stderr!r}")

            mode = stat.S_IMODE(status_socket.stat().st_mode)
            if mode != 0o600:
                raise RuntimeError(f"status socket mode is {oct(mode)}, expected 0o600")

            initial = query_status(args.fps_server, config_path, status_socket)
            if initial["role"] != "server" or initial["tun"]["enabled"]:
                raise RuntimeError(f"unexpected initial status: {initial!r}")
            if initial["sessions"]["accepted"] != 0:
                raise RuntimeError(f"status should start with no sessions: {initial!r}")
            for section in ["auth", "classified_record"]:
                if section not in initial or not isinstance(initial[section], dict):
                    raise RuntimeError(f"status missing {section} section: {initial!r}")
            shaper = initial.get("shaper")
            if not shaper or not shaper.get("enabled") or "profile" not in shaper:
                raise RuntimeError(f"status missing shaper profile snapshot: {initial!r}")
            profile = shaper["profile"]
            if profile.get("profile_id") != "status-smoke":
                raise RuntimeError(f"unexpected shaper profile id: {profile!r}")
            if profile.get("record_size_cdf_c2s") != [[4096, 1.0]]:
                raise RuntimeError(f"unexpected compact record CDF: {profile!r}")
            if "inter_record_delay_ms_cdf_c2s" in profile:
                raise RuntimeError(f"legacy millisecond delay CDF present: {profile!r}")

            with socket.create_connection(("127.0.0.1", listen_port), timeout=2.0):
                pass

            def accepted_and_closed():
                status = query_status(args.fps_server, config_path, status_socket)
                sessions = status["sessions"]
                if sessions["accepted"] >= 1 and sessions["closed"] >= 1:
                    return status
                return None

            status = wait_for(accepted_and_closed)
            if status is None:
                raise RuntimeError("status counters did not observe accepted/closed session")
            if status["sessions"]["active"] != 0:
                raise RuntimeError(f"dead target session stayed active: {status!r}")
            if status["auth"]["authenticated"] != 0:
                raise RuntimeError(f"passthrough status unexpectedly authenticated: {status!r}")
            if "authenticated" in status["sessions"]:
                raise RuntimeError(f"legacy session authenticated field still present: {status!r}")
            recent = status["sessions"].get("recent_closed")
            last = status["sessions"].get("last_closed")
            if not isinstance(recent, list) or not recent:
                raise RuntimeError(f"status missing recent closed sessions: {status!r}")
            if not isinstance(last, dict):
                raise RuntimeError(f"status missing last closed session: {status!r}")
            if recent[-1] != last:
                raise RuntimeError(f"last_closed does not match recent tail: {status!r}")
            close = last.get("close")
            if not isinstance(close, dict):
                raise RuntimeError(f"last_closed missing close metadata: {status!r}")
            if close.get("reason") != "tcp_error":
                raise RuntimeError(f"unexpected close reason: {status!r}")
            if close.get("component") != "tcp":
                raise RuntimeError(f"unexpected close component: {status!r}")
            if close.get("error") != "target_connect_failed":
                raise RuntimeError(f"unexpected close error: {status!r}")
            if last.get("authenticated"):
                raise RuntimeError(f"dead target status should not authenticate: {status!r}")
            text = json.dumps(status)
            forbidden = [
                "client_uuid",
                "server_private_key",
                "allowed_client_uuids",
                "ClientID",
                "client_instance_id",
            ]
            if any(item in text for item in forbidden):
                raise RuntimeError(f"status leaked sensitive fields: {status!r}")
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)

    return 0


if __name__ == "__main__":
    sys.exit(main())
