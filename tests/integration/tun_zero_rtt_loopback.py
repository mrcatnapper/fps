#!/usr/bin/env python3

import argparse
import os
import select
import shlex
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
from pathlib import Path

from fps_https_harness import (
    ZERO_RTT_CLIENT_UUID,
    ZERO_RTT_SERVER_PRIVATE_BASE64,
    ZERO_RTT_SERVER_PUBLIC_BASE64,
    assert_log_contains,
    assert_log_omits,
    free_port,
    prepare_zero_rtt_fixture_dir,
)


DEFAULT_CARRIER = str(Path(__file__).resolve().parents[2] / "tools" / "fps_carrier.py")
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


def preflight():
    if not Path("/dev/net/tun").exists():
        return "missing /dev/net/tun"
    if shutil_which("ip") is None:
        return "missing ip executable"
    if shutil_which("openssl") is None:
        return "missing openssl executable"
    if not has_cap_net_admin():
        return "missing CAP_NET_ADMIN/root for netns and TUN setup"
    netns_probe = f"fpsprobe{os.getpid()}"
    completed = subprocess.run(
        ["ip", "netns", "add", netns_probe],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        return "cannot create network namespace: " + completed.stderr.strip()
    subprocess.run(
        ["ip", "netns", "del", netns_probe],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return None


def shutil_which(name):
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        candidate = Path(directory) / name
        if candidate.exists() and os.access(candidate, os.X_OK):
            return str(candidate)
    return None


def run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


def run_ns(namespace, args, **kwargs):
    return run(["ip", "netns", "exec", namespace, *args], **kwargs)


def start(args):
    return subprocess.Popen(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )


def carrier_command(carrier):
    if carrier.endswith(".py"):
        return [sys.executable, carrier]
    return [carrier]


def unconfined_prefix():
    if shutil_which("aa-exec") is None:
        return []
    return ["aa-exec", "-p", "unconfined", "--"]


def terminate(process, initial_signal=signal.SIGTERM):
    if process is None:
        return
    if process.poll() is not None:
        return
    try:
        try:
            os.killpg(process.pid, initial_signal)
        except PermissionError:
            if shutil_which("aa-exec") is not None:
                subprocess.run(
                    [
                        "aa-exec",
                        "-p",
                        "unconfined",
                        "--",
                        "kill",
                        f"-{initial_signal.value}",
                        f"-{process.pid}",
                    ],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
        process.wait(timeout=5.0)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except PermissionError:
                if shutil_which("aa-exec") is not None:
                    subprocess.run(
                        [
                            "aa-exec",
                            "-p",
                            "unconfined",
                            "--",
                            "kill",
                            "-KILL",
                            f"-{process.pid}",
                        ],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
        except ProcessLookupError:
            pass
        process.wait(timeout=5.0)


def stop_and_read(process):
    if process is None:
        return ""
    terminate(process)
    if process.stdout is None:
        return ""
    return process.stdout.read()


def assert_alive(process, name):
    if process.poll() is None:
        return
    output = process.stdout.read() if process.stdout is not None else ""
    raise RuntimeError(f"{name} exited early with {process.returncode}: {output}")


def wait_for_line(process, name, expected="READY", timeout=5.0):
    deadline = time.monotonic() + timeout
    collected = []
    while time.monotonic() < deadline:
        assert_alive(process, name)
        ready, _, _ = select.select([process.stdout], [], [], 0.1)
        if not ready:
            continue
        line = process.stdout.readline()
        if not line:
            continue
        collected.append(line.rstrip())
        if expected in line:
            return
    raise RuntimeError(f"timed out waiting for {name} {expected}: {collected!r}")


def wait_for_tcp_ns(namespace, host, port, process, name, timeout=5.0):
    probe = (
        "import socket,sys; "
        f"s=socket.create_connection(({host!r},{port}), timeout=0.3); "
        "s.close()"
    )
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        assert_alive(process, name)
        completed = subprocess.run(
            ["ip", "netns", "exec", namespace, "python3", "-c", probe],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode == 0:
            return
        last_error = completed.stderr.strip()
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {host}:{port} in {namespace}: {last_error}")


def wait_for_link(namespace, name, process, proc_name, timeout=5.0):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        assert_alive(process, proc_name)
        completed = subprocess.run(
            ["ip", "netns", "exec", namespace, "ip", "link", "show", "dev", name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode == 0:
            return
        last_error = completed.stderr.strip()
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for TUN {name} in {namespace}: {last_error}")


def wait_for_address(namespace, name, address, process, proc_name, timeout=5.0):
    deadline = time.monotonic() + timeout
    last_output = ""
    while time.monotonic() < deadline:
        assert_alive(process, proc_name)
        completed = subprocess.run(
            ["ip", "netns", "exec", namespace, "ip", "addr", "show", "dev", name],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        last_output = completed.stdout
        if completed.returncode == 0 and address in completed.stdout:
            return
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {address} on {name}: {last_output}")


def shaper_json(enabled):
    if not enabled:
        return ""
    return """
  ,
  "shaper": {
    "enabled": true,
    "profile_id": "tun-loopback-test",
    "record_size_cdf_c2s": [{"le": 4096, "p": 1.0}],
    "record_size_cdf_s2c": [{"le": 4096, "p": 1.0}],
    "inter_record_delay_ms_cdf_c2s": [{"le": 1, "p": 1.0}],
    "inter_record_delay_ms_cdf_s2c": [{"le": 1, "p": 1.0}],
    "covert_ratio_max": 1.0,
    "burst_records_max": 64,
    "jitter_ms": {"min": 0, "max": 0},
    "deterministic_seed": 7
  }
"""


def zero_rtt_json(role):
    if role == "client":
        key_config = """
      "client_uuid": "%s",
      "server_public_key_base64": "%s",
""" % (ZERO_RTT_CLIENT_UUID, ZERO_RTT_SERVER_PUBLIC_BASE64)
    elif role == "server":
        key_config = """
      "server_private_key_base64": "%s",
      "server_public_key_base64": "%s",
      "allowed_client_uuids": ["%s"],
""" % (ZERO_RTT_SERVER_PRIVATE_BASE64, ZERO_RTT_SERVER_PUBLIC_BASE64, ZERO_RTT_CLIENT_UUID)
    else:
        raise ValueError(f"unsupported role: {role}")

    return """
  "security": {
    "zero_rtt": {
      "enabled": true,
      "profile_id": "tun-loopback-v4",
%s      "version": 4,
      "capabilities": 1,
      "max_padding_size": 64,
      "min_records_before_trial": 1,
      "upgrade_direction": "client_to_server"
    }
  },
""" % key_config


def write_json_config(
    path,
    listen,
    target_name,
    target,
    role,
    tun_name,
    enable_shaper,
    tun_mtu,
    codec_max_frame_payload,
    lease_file=None,
):
    if role == "server":
        lease_json = """
    ,
    "lease_pool": "10.77.0.0/30",
    "server_address": "10.77.0.1",
    "lease_file": "%s"
""" % lease_file
    elif role == "client":
        lease_json = """
    ,
    "auto_configure": true
"""
    else:
        raise ValueError(f"unsupported role: {role}")

    path.write_text(
        """
{
  "network": {
    "listen": "%s",
    "%s": "%s",
    "read_buffer_size": 4096
  },
%s
  "codec": {
    "max_frame_payload": %d,
    "max_frame_padding": 64,
    "allow_fragmentation": true
  },
  "tun": {
    "enabled": true,
    "name": "%s",
    "mtu": %d,
    "max_write_queue_packets": 64
%s
  }
  %s
}
"""
        % (
            listen,
            target_name,
            target,
            zero_rtt_json(role),
            codec_max_frame_payload,
            tun_name,
            tun_mtu,
            lease_json,
            shaper_json(enable_shaper),
        ),
        encoding="utf-8",
    )


def write_udp_echo_script(path):
    path.write_text(
        textwrap.dedent(
            r"""
            import argparse
            import socket

            parser = argparse.ArgumentParser()
            parser.add_argument("--bind", required=True)
            parser.add_argument("--port", type=int, required=True)
            parser.add_argument("--count", type=int, default=1)
            args = parser.parse_args()

            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind((args.bind, args.port))
            print("READY", flush=True)
            for _ in range(args.count):
                data, peer = sock.recvfrom(4096)
                sock.sendto(data, peer)
            """
        ),
        encoding="utf-8",
    )


def run_udp_probe(namespace, port, count, payload_size):
    code = textwrap.dedent(
        f"""
        import socket

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(5.0)
        sock.bind(("10.77.0.2", 0))
        for index in range({count}):
            payload = bytes((index + offset) % 256 for offset in range({payload_size}))
            sock.sendto(payload, ("10.77.0.1", {port}))
            data, _ = sock.recvfrom(4096)
            if data != payload:
                raise RuntimeError((index, len(data), len(payload)))
        """
    )
    run_ns(namespace, ["python3", "-c", code])


def setup_namespaces(client_ns, server_ns, client_veth, server_veth):
    run(["ip", "netns", "add", client_ns])
    run(["ip", "netns", "add", server_ns])
    run(["ip", "link", "add", client_veth, "type", "veth", "peer", "name", server_veth])
    run(["ip", "link", "set", client_veth, "netns", client_ns])
    run(["ip", "link", "set", server_veth, "netns", server_ns])
    run_ns(client_ns, ["ip", "link", "set", "lo", "up"])
    run_ns(server_ns, ["ip", "link", "set", "lo", "up"])
    run_ns(client_ns, ["ip", "addr", "add", "192.0.2.1/30", "dev", client_veth])
    run_ns(server_ns, ["ip", "addr", "add", "192.0.2.2/30", "dev", server_veth])
    run_ns(client_ns, ["ip", "link", "set", client_veth, "up"])
    run_ns(server_ns, ["ip", "link", "set", server_veth, "up"])


def cleanup_namespaces(*namespaces):
    for namespace in namespaces:
        subprocess.run(
            ["ip", "netns", "del", namespace],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps-client", required=True)
    parser.add_argument("--fps-server", required=True)
    parser.add_argument("--carrier", default=DEFAULT_CARRIER)
    parser.add_argument("--carrier-count", type=int, default=1)
    parser.add_argument("--expect-carrier-count", type=int, default=0)
    parser.add_argument("--udp-count", type=int, default=1)
    parser.add_argument("--udp-payload-size", type=int, default=len(b"fps-tun-smoke"))
    parser.add_argument("--tun-mtu", type=int, default=1280)
    parser.add_argument("--codec-max-frame-payload", type=int, default=1280)
    parser.add_argument("--cover-padding-size", type=int, default=0)
    parser.add_argument("--enable-shaper", action="store_true")
    parser.add_argument("--capture-pcap")
    parser.add_argument("--capture-interface", default="underlay")
    parser.add_argument("--capture-filter")
    args = parser.parse_args()

    if args.udp_count <= 0:
        raise ValueError("--udp-count must be positive")
    if args.carrier_count <= 0:
        raise ValueError("--carrier-count must be positive")
    if args.expect_carrier_count < 0:
        raise ValueError("--expect-carrier-count must not be negative")
    if args.udp_payload_size <= 0:
        raise ValueError("--udp-payload-size must be positive")
    if args.tun_mtu <= 0:
        raise ValueError("--tun-mtu must be positive")
    if args.codec_max_frame_payload <= 0:
        raise ValueError("--codec-max-frame-payload must be positive")
    if args.cover_padding_size < 0:
        raise ValueError("--cover-padding-size must not be negative")
    if args.capture_pcap and shutil_which("tcpdump") is None:
        return skip("missing tcpdump executable")

    reason = preflight()
    if reason is not None:
        return skip(reason)

    suffix = str(os.getpid())
    client_ns = f"fpsc{suffix}"
    server_ns = f"fpss{suffix}"
    client_veth = f"vc{suffix[-8:]}"
    server_veth = f"vs{suffix[-8:]}"
    origin = None
    server = None
    client = None
    covers = []
    udp_echo = None
    capture = None
    server_log = ""
    client_log = ""
    capture_log = ""

    with tempfile.TemporaryDirectory() as tmpdir_str:
        tmpdir = Path(tmpdir_str)
        prepare_zero_rtt_fixture_dir(tmpdir)

        udp_echo_script = tmpdir / "udp_echo.py"
        write_udp_echo_script(udp_echo_script)

        cert = tmpdir / "carrier-cert.pem"
        key = tmpdir / "carrier-key.pem"
        origin_port = free_port()
        server_port = free_port()
        client_port = free_port()
        udp_port = free_port()
        server_config = tmpdir / "server.json"
        client_config = tmpdir / "client.json"
        write_json_config(
            server_config,
            f"192.0.2.2:{server_port}",
            "origin",
            f"127.0.0.1:{origin_port}",
            "server",
            "fpss0",
            args.enable_shaper,
            args.tun_mtu,
            args.codec_max_frame_payload,
            str(tmpdir / "leases.json"),
        )
        write_json_config(
            client_config,
            f"127.0.0.1:{client_port}",
            "server",
            f"192.0.2.2:{server_port}",
            "client",
            "fpsc0",
            args.enable_shaper,
            args.tun_mtu,
            args.codec_max_frame_payload,
        )

        try:
            setup_namespaces(client_ns, server_ns, client_veth, server_veth)
            origin = start(
                [
                    "ip",
                    "netns",
                    "exec",
                    server_ns,
                    *carrier_command(args.carrier),
                    "origin",
                    "--listen",
                    f"127.0.0.1:{origin_port}",
                    "--cert",
                    str(cert),
                    "--key",
                    str(key),
                    "--generate-self-signed",
                    "--path",
                    "/fps-carrier",
                    "--max-frame-size",
                    str(max(16384, args.cover_padding_size + 256)),
                ]
            )
            wait_for_line(origin, "carrier origin")

            server_args = [
                "ip",
                "netns",
                "exec",
                server_ns,
                args.fps_server,
                "--config",
                str(server_config),
            ]
            client_args = [
                "ip",
                "netns",
                "exec",
                client_ns,
                args.fps_client,
                "--config",
                str(client_config),
            ]
            if args.expect_carrier_count > 0:
                server_args.extend(["--log-level", "debug"])
                client_args.extend(["--log-level", "debug"])

            server = start(server_args)
            wait_for_tcp_ns(server_ns, "192.0.2.2", server_port, server, "fps_server")
            wait_for_link(server_ns, "fpss0", server, "fps_server")
            run_ns(server_ns, ["ip", "addr", "add", "10.77.0.1/30", "dev", "fpss0"])
            run_ns(server_ns, ["ip", "link", "set", "fpss0", "up", "mtu", str(args.tun_mtu)])

            client = start(client_args)
            wait_for_tcp_ns(client_ns, "127.0.0.1", client_port, client, "fps_client")
            wait_for_link(client_ns, "fpsc0", client, "fps_client")

            if args.capture_pcap:
                capture_path = Path(args.capture_pcap)
                capture_path.parent.mkdir(parents=True, exist_ok=True)
                capture_interface = (
                    server_veth if args.capture_interface == "underlay" else args.capture_interface
                )
                capture_filter = (
                    shlex.split(args.capture_filter)
                    if args.capture_filter
                    else ["tcp", "port", str(server_port)]
                )
                capture = start(
                    [
                        "ip",
                        "netns",
                        "exec",
                        server_ns,
                        *unconfined_prefix(),
                        "tcpdump",
                        "--immediate-mode",
                        "-i",
                        capture_interface,
                        "-s",
                        "0",
                        "-U",
                        "-w",
                        str(capture_path),
                        *capture_filter,
                    ]
                )
                wait_for_line(capture, "tcpdump", expected="listening")

            carrier_bps = max(4096, args.cover_padding_size * 4)
            for index in range(args.carrier_count):
                cover = start(
                    [
                        "ip",
                        "netns",
                        "exec",
                        client_ns,
                        *carrier_command(args.carrier),
                        "client",
                        "--connect",
                        f"127.0.0.1:{client_port}",
                        "--path",
                        "/fps-carrier",
                        "--client-bps",
                        str(carrier_bps),
                        "--frame-rate",
                        "4",
                        "--seed",
                        str(1000 + index),
                        "--max-frame-size",
                        str(max(16384, args.cover_padding_size + 256)),
                        "--ready-file",
                        str(tmpdir / f"carrier-{index}.ready"),
                    ]
                )
                covers.append(cover)
                wait_for_line(cover, "carrier client")
            wait_for_address(client_ns, "fpsc0", "10.77.0.2/30", client, "fps_client")

            udp_echo = start(
                [
                    "ip",
                    "netns",
                    "exec",
                    server_ns,
                    "python3",
                    str(udp_echo_script),
                    "--bind",
                    "10.77.0.1",
                    "--port",
                    str(udp_port),
                    "--count",
                    str(args.udp_count),
                ]
            )
            wait_for_line(udp_echo, "UDP echo")
            run_udp_probe(client_ns, udp_port, args.udp_count, args.udp_payload_size)
            for cover in covers:
                assert_alive(cover, "carrier client")
        finally:
            terminate(udp_echo)
            for cover in covers:
                terminate(cover)
            client_log = stop_and_read(client)
            server_log = stop_and_read(server)
            terminate(origin)
            if capture is not None:
                time.sleep(0.5)
            terminate(capture, signal.SIGINT)
            if capture is not None and capture.stdout is not None:
                capture_log = capture.stdout.read()
            cleanup_namespaces(client_ns, server_ns)

    logs = client_log + "\n" + server_log
    assert_log_contains(logs, "event=zero_rtt_authenticated")
    assert_log_omits(logs, "session_key")
    assert_log_omits(logs, "raw_payload")
    if args.expect_carrier_count > 0:
        assert_log_contains(logs, "event=carrier_registered source=zero_rtt")
        assert_log_contains(logs, f"carrier_count={args.expect_carrier_count}")
    if args.capture_pcap and Path(args.capture_pcap).stat().st_size <= 24:
        raise RuntimeError(f"capture pcap is empty; tcpdump output: {capture_log!r}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
