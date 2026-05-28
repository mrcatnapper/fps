#!/usr/bin/env python3

import argparse
import json
import select
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from fps_https_harness import (
    ZERO_RTT_DECOY_CLIENT_UUIDS,
    assert_roundtrip_bodies,
    free_port,
    generate_cert,
    https_get_roundtrips,
    prepare_zero_rtt_fixture_dir,
    start_https_origin,
    start_process,
    stop_and_read,
    stop_origin,
    wait_for_tcp,
    write_zero_rtt_relay_config,
)


def run_command(args):
    return subprocess.run(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def wait_for(predicate, timeout=6.0, interval=0.05):
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
            f"status query failed: rc={completed.returncode} stderr={completed.stderr!r}"
        )
    return json.loads(completed.stdout)


class TlsRecordTransform:
    def __init__(self, on_record):
        self.pending = bytearray()
        self.on_record = on_record

    def feed(self, data):
        self.pending.extend(data)
        out = bytearray()
        while len(self.pending) >= 5:
            length = int.from_bytes(self.pending[3:5], "big")
            end = 5 + length
            if len(self.pending) < end:
                break
            record = bytes(self.pending[:end])
            del self.pending[:end]
            out.extend(self.on_record(record))
        return bytes(out)

    def flush(self):
        out = bytes(self.pending)
        self.pending.clear()
        return out


class RecordingProxy:
    def __init__(self, listen_port, target_port, tamper_post_auth=False):
        self.listen_port = listen_port
        self.target_port = target_port
        self.tamper_post_auth = tamper_post_auth
        self.c2s_records = []
        self.s2c_records = []
        self.seen_upgrade = threading.Event()
        self.tampered = threading.Event()
        self._stop = threading.Event()
        self._ready = threading.Event()
        self._threads = []
        self._listener = None

    def start(self):
        thread = threading.Thread(target=self._run, daemon=True)
        thread.start()
        self._threads.append(thread)
        if not self._ready.wait(timeout=5.0):
            raise RuntimeError("recording proxy did not become ready")

    def stop(self):
        self._stop.set()
        if self._listener is not None:
            try:
                self._listener.close()
            except OSError:
                pass
        try:
            with socket.create_connection(("127.0.0.1", self.listen_port), timeout=0.2):
                pass
        except OSError:
            pass
        for thread in self._threads:
            thread.join(timeout=2.0)

    def _run(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", self.listen_port))
            listener.listen()
            listener.settimeout(0.2)
            self._listener = listener
            self._ready.set()
            while not self._stop.is_set():
                try:
                    client, _ = listener.accept()
                except TimeoutError:
                    continue
                except OSError:
                    break
                try:
                    upstream = socket.create_connection(("127.0.0.1", self.target_port), timeout=3.0)
                except OSError:
                    client.close()
                    continue
                self._start_connection(client, upstream)

    def _start_connection(self, client, upstream):
        c2s = TlsRecordTransform(self._on_c2s_record)
        s2c = TlsRecordTransform(self._on_s2c_record)
        for src, dst, transform in [(client, upstream, c2s), (upstream, client, s2c)]:
            thread = threading.Thread(
                target=self._pipe,
                args=(src, dst, transform),
                daemon=True,
            )
            thread.start()
            self._threads.append(thread)

    def _pipe(self, src, dst, transform):
        with src, dst:
            try:
                while not self._stop.is_set():
                    try:
                        ready, _, _ = select.select([src], [], [], 0.2)
                    except ValueError:
                        return
                    if not ready:
                        continue
                    data = src.recv(65536)
                    if not data:
                        tail = transform.flush()
                        if tail:
                            dst.sendall(tail)
                        try:
                            dst.shutdown(socket.SHUT_WR)
                        except OSError:
                            pass
                        return
                    out = transform.feed(data)
                    if out:
                        dst.sendall(out)
            except OSError:
                return

    def _on_c2s_record(self, record):
        self.c2s_records.append(record)
        if len(self.c2s_records) >= 2:
            self.seen_upgrade.set()
        return record

    def _on_s2c_record(self, record):
        self.s2c_records.append(record)
        if (
            self.tamper_post_auth
            and self.seen_upgrade.is_set()
            and record[:1] == b"\x17"
            and len([item for item in self.s2c_records if item[:1] == b"\x17"]) >= 2
            and not self.tampered.is_set()
        ):
            tampered = bytearray(record)
            tampered[-1] ^= 0x01
            self.tampered.set()
            return bytes(tampered)
        return record


def start_zero_rtt_server(fps_server, config_path):
    process = start_process([fps_server, "--config", str(config_path), "--log-level", "debug"])
    return process


def start_zero_rtt_client(fps_client, config_path):
    process = start_process([fps_client, "--config", str(config_path), "--log-level", "debug"])
    return process


def wait_for_status_counter(binary, config_path, socket_path, section, name, minimum=1):
    def read():
        status = query_status(binary, config_path, socket_path)
        if int(status.get(section, {}).get(name, 0)) >= minimum:
            return status
        return None

    status = wait_for(read)
    if status is None:
        raise RuntimeError(f"status counter {section}.{name} did not reach {minimum}")
    return status


def assert_no_status_secrets(status):
    text = json.dumps(status)
    forbidden = [
        "client_uuid",
        "server_private_key",
        "allowed_client_uuids",
        "ClientID",
        "session_key",
        "raw_payload",
    ]
    leaked = [item for item in forbidden if item in text]
    if leaked:
        raise RuntimeError(f"status leaked sensitive markers {leaked}: {status!r}")


def direct_passthrough_probe(fps_server, tmpdir, origin_port):
    server_port = free_port()
    status_socket = tmpdir / "direct.status"
    server_config = tmpdir / "direct-server.json"
    write_zero_rtt_relay_config(
        server_config,
        server_port,
        "origin",
        origin_port,
        "server",
        status_socket=status_socket,
    )
    server = start_zero_rtt_server(fps_server, server_config)
    try:
        wait_for_tcp("127.0.0.1", server_port, server)
        bodies = https_get_roundtrips(server_port)
        assert_roundtrip_bodies(bodies)
        status = wait_for_status_counter(
            fps_server, server_config, status_socket, "auth", "candidates"
        )
        if int(status.get("auth", {}).get("authenticated", 0)) != 0:
            raise RuntimeError(f"direct passthrough unexpectedly authenticated: {status!r}")
        assert_no_status_secrets(status)
    finally:
        stop_and_read(server)


def unknown_client_probe(fps_client, fps_server, tmpdir, origin_port):
    server_port = free_port()
    client_port = free_port()
    server_status_socket = tmpdir / "unknown-server.status"
    server_config = tmpdir / "unknown-server.json"
    client_config = tmpdir / "unknown-client.json"
    write_zero_rtt_relay_config(
        server_config,
        server_port,
        "origin",
        origin_port,
        "server",
        status_socket=server_status_socket,
    )
    write_zero_rtt_relay_config(
        client_config,
        client_port,
        "server",
        server_port,
        "client",
        client_uuid=ZERO_RTT_DECOY_CLIENT_UUIDS[0],
    )
    server = start_zero_rtt_server(fps_server, server_config)
    client = None
    try:
        wait_for_tcp("127.0.0.1", server_port, server)
        client = start_zero_rtt_client(fps_client, client_config)
        wait_for_tcp("127.0.0.1", client_port, client)
        try:
            https_get_roundtrips(client_port, paths=["/unknown-client"])
        except (OSError, ssl.SSLError, RuntimeError):
            pass
        status = wait_for_status_counter(
            fps_server, server_config, server_status_socket, "auth", "unknown_client"
        )
        assert_no_status_secrets(status)
    finally:
        stop_and_read(client)
        stop_and_read(server)


def transcript_mismatch_probe(fps_client, fps_server, tmpdir, origin_port):
    server_port = free_port()
    proxy_port = free_port()
    client_port = free_port()
    server_status_socket = tmpdir / "transcript-server.status"
    server_config = tmpdir / "transcript-server.json"
    client_config = tmpdir / "transcript-client.json"
    write_zero_rtt_relay_config(
        server_config,
        server_port,
        "origin",
        origin_port,
        "server",
        status_socket=server_status_socket,
    )
    write_zero_rtt_relay_config(client_config, client_port, "server", proxy_port, "client")
    server = start_zero_rtt_server(fps_server, server_config)
    proxy = RecordingProxy(proxy_port, server_port)
    client = None
    try:
        wait_for_tcp("127.0.0.1", server_port, server)
        proxy.start()
        client = start_zero_rtt_client(fps_client, client_config)
        wait_for_tcp("127.0.0.1", client_port, client)
        bodies = https_get_roundtrips(client_port, paths=["/transcript"])
        assert_roundtrip_bodies(bodies, paths=["/transcript"])
        wait_for_status_counter(
            fps_server, server_config, server_status_socket, "auth", "authenticated"
        )
        if not wait_for(lambda: len(proxy.c2s_records) >= 2):
            raise RuntimeError(f"proxy did not record enough c2s records: {proxy.c2s_records!r}")
        precheck_before = int(
            query_status(fps_server, server_config, server_status_socket)
            .get("auth", {})
            .get("precheck_failed", 0)
        )
        status = None
        mutated_prefix = bytearray(proxy.c2s_records[0])
        if len(mutated_prefix) > 5:
            mutated_prefix[-1] ^= 0x01
        for record_index in range(1, min(len(proxy.c2s_records), 12)):
            replay_bytes = bytes(mutated_prefix) + b"".join(proxy.c2s_records[1 : record_index + 1])
            try:
                with socket.create_connection(("127.0.0.1", server_port), timeout=3.0) as sock:
                    sock.sendall(replay_bytes)
                    time.sleep(0.2)
            except OSError:
                pass
            candidate_status = query_status(fps_server, server_config, server_status_socket)
            if int(candidate_status.get("auth", {}).get("precheck_failed", 0)) > precheck_before:
                status = candidate_status
                break
        if status is None:
            summary = [
                [record[0], int.from_bytes(record[3:5], "big")]
                for record in proxy.c2s_records[:12]
            ]
            raise RuntimeError(f"transcript mismatch did not trigger precheck failure; c2s records={summary!r}")
        assert_no_status_secrets(status)
    finally:
        stop_and_read(client)
        proxy.stop()
        stop_and_read(server)


def tampered_envelope_probe(fps_client, fps_server, tmpdir, origin_port):
    server_port = free_port()
    proxy_port = free_port()
    client_port = free_port()
    server_status_socket = tmpdir / "tamper-server.status"
    client_status_socket = tmpdir / "tamper-client.status"
    server_config = tmpdir / "tamper-server.json"
    client_config = tmpdir / "tamper-client.json"
    write_zero_rtt_relay_config(
        server_config,
        server_port,
        "origin",
        origin_port,
        "server",
        status_socket=server_status_socket,
    )
    write_zero_rtt_relay_config(
        client_config,
        client_port,
        "server",
        proxy_port,
        "client",
        status_socket=client_status_socket,
    )
    server = start_zero_rtt_server(fps_server, server_config)
    proxy = RecordingProxy(proxy_port, server_port, tamper_post_auth=True)
    client = None
    try:
        wait_for_tcp("127.0.0.1", server_port, server)
        proxy.start()
        client = start_zero_rtt_client(fps_client, client_config)
        wait_for_tcp("127.0.0.1", client_port, client)
        try:
            https_get_roundtrips(client_port, paths=["/tamper"])
        except (OSError, ssl.SSLError, RuntimeError):
            pass
        if not proxy.tampered.wait(timeout=5.0):
            raise RuntimeError("proxy did not tamper with a post-auth server-to-client record")
        status = wait_for_status_counter(
            fps_client, client_config, client_status_socket, "envelope", "decode_failed"
        )
        if int(status.get("envelope", {}).get("tampered_or_invalid", 0)) < 1:
            raise RuntimeError(f"tamper counter did not increment: {status!r}")
        recent = status.get("sessions", {}).get("recent_closed", [])
        if not recent:
            raise RuntimeError(f"missing recent close metadata after tamper: {status!r}")
        assert_no_status_secrets(status)
    finally:
        stop_and_read(client)
        proxy.stop()
        stop_and_read(server)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps-client", required=True)
    parser.add_argument("--fps-server", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="fps-zero-rtt-adversarial-") as tmpdir_str:
        tmpdir = Path(tmpdir_str)
        cert, key = generate_cert(tmpdir)
        prepare_zero_rtt_fixture_dir(tmpdir)
        origin_port = free_port()
        origin = start_https_origin(cert, key, origin_port)
        try:
            direct_passthrough_probe(args.fps_server, tmpdir, origin_port)
            unknown_client_probe(args.fps_client, args.fps_server, tmpdir, origin_port)
            transcript_mismatch_probe(args.fps_client, args.fps_server, tmpdir, origin_port)
            tampered_envelope_probe(args.fps_client, args.fps_server, tmpdir, origin_port)
        finally:
            stop_origin(origin)

    return 0


if __name__ == "__main__":
    sys.exit(main())
