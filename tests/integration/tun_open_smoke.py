#!/usr/bin/env python3

import argparse
import os
import select
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from fps_https_harness import (
    ZERO_RTT_CLIENT_UUID,
    ZERO_RTT_SERVER_PRIVATE_BASE64,
    ZERO_RTT_SERVER_PUBLIC_BASE64,
    prepare_zero_rtt_fixture_dir,
)


SKIP = 77
CAP_NET_ADMIN = 12


def skip(reason):
    print(f"SKIP: {reason}", file=sys.stderr)
    return SKIP


def has_cap_net_admin():
    try:
        for line in Path("/proc/self/status").read_text(encoding="ascii").splitlines():
            if line.startswith("CapEff:"):
                value = int(line.split()[1], 16)
                return (value & (1 << CAP_NET_ADMIN)) != 0
    except OSError:
        return False
    return False


def which(name):
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        candidate = Path(directory) / name
        if candidate.exists() and os.access(candidate, os.X_OK):
            return str(candidate)
    return None


def preflight():
    if not Path("/dev/net/tun").exists():
        return "missing /dev/net/tun"
    if which("ip") is None:
        return "missing ip executable"
    if not has_cap_net_admin():
        return "missing CAP_NET_ADMIN/root for TUN setup"
    return None


def start(args):
    return subprocess.Popen(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )


def terminate(process):
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5.0)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=5.0)


def link_exists(name):
    return (
        subprocess.run(
            ["ip", "link", "show", "dev", name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode
        == 0
    )


def wait_for_open(process, tun_name, timeout=5.0):
    deadline = time.monotonic() + timeout
    collected = []
    while time.monotonic() < deadline:
        if link_exists(tun_name):
            return
        if process.poll() is not None:
            output = process.stdout.read() if process.stdout is not None else ""
            raise RuntimeError(
                f"fps_server exited early with {process.returncode}: "
                f"{collected!r} {output}"
            )
        ready, _, _ = select.select([process.stdout], [], [], 0.1)
        if not ready:
            continue
        line = process.stdout.readline()
        if not line:
            continue
        collected.append(line.rstrip())
    raise RuntimeError(f"timed out waiting for TUN link {tun_name}: {collected!r}")


def write_config(path, tun_name):
    path.write_text(
        """
{
  "network": {
    "listen": "127.0.0.1:0",
    "origin": "127.0.0.1:1",
    "read_buffer_size": 4096
  },
  "security": {
    "zero_rtt": {
      "enabled": true,
      "profile_id": "tun-open-smoke-v3",
      "server_private_key_base64": "%s",
      "server_public_key_base64": "%s",
      "allowed_client_uuids": ["%s"]
    }
  },
  "codec": {
    "max_frame_payload": 1280,
    "max_frame_padding": 64
  },
  "tun": {
    "enabled": true,
    "name": "%s",
    "mtu": 1280,
    "max_write_queue_packets": 4
  }
}
"""
        % (ZERO_RTT_SERVER_PRIVATE_BASE64, ZERO_RTT_SERVER_PUBLIC_BASE64, ZERO_RTT_CLIENT_UUID, tun_name),
        encoding="utf-8",
    )


def cleanup_link(name):
    subprocess.run(
        ["ip", "link", "del", "dev", name],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps-server", required=True)
    args = parser.parse_args()

    reason = preflight()
    if reason is not None:
        return skip(reason)

    tun_name = f"fpsop{str(os.getpid())[-8:]}"
    process = None
    with tempfile.TemporaryDirectory() as tmpdir_str:
        tmpdir = Path(tmpdir_str)
        prepare_zero_rtt_fixture_dir(tmpdir)
        config = tmpdir / "server.json"
        write_config(config, tun_name)

        try:
            process = start([args.fps_server, "--config", str(config)])
            wait_for_open(process, tun_name)
            if not link_exists(tun_name):
                raise RuntimeError(f"TUN link {tun_name} did not appear")
        finally:
            terminate(process)
            if link_exists(tun_name):
                cleanup_link(tun_name)
                raise RuntimeError(f"TUN link {tun_name} survived process shutdown")

    return 0


if __name__ == "__main__":
    sys.exit(main())
