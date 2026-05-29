#!/usr/bin/env python3

import argparse
import socket
import ssl
import sys
import tempfile
import threading
from pathlib import Path

from fps_https_harness import (
    free_port,
    generate_cert,
    read_http_response,
    response_body,
    start_https_origin,
    stop_origin,
    prepare_zero_rtt_fixture_dir,
    ZeroRttRelayPair,
)


def https_worker(port, paths, barrier, results, errors, index):
    try:
        context = ssl._create_unverified_context()
        bodies = []
        with socket.create_connection(("127.0.0.1", port), timeout=5.0) as raw:
            raw.settimeout(5.0)
            with context.wrap_socket(raw, server_hostname="localhost") as sock:
                sock.settimeout(5.0)
                fileobj = sock.makefile("rb")
                barrier.wait(timeout=5.0)
                for path in paths:
                    request = (
                        f"GET {path} HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Connection: keep-alive\r\n"
                        "\r\n"
                    ).encode("ascii")
                    sock.sendall(request)
                    bodies.append(read_http_response(fileobj))
        results[index] = bodies
    except BaseException as error:
        errors[index] = error


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps-client", required=True)
    parser.add_argument("--fps-server", required=True)
    args = parser.parse_args()

    logs = ""
    with tempfile.TemporaryDirectory() as tmpdir_str:
        tmpdir = Path(tmpdir_str)
        cert, key = generate_cert(tmpdir)
        prepare_zero_rtt_fixture_dir(tmpdir)

        origin_port = free_port()
        server_port = free_port()
        client_port = free_port()
        origin = start_https_origin(cert, key, origin_port)

        try:
            with ZeroRttRelayPair(
                tmpdir=tmpdir,
                fps_client=args.fps_client,
                fps_server=args.fps_server,
                origin_port=origin_port,
                server_port=server_port,
                client_port=client_port,
            ) as relays:
                paths = [
                    ["/v5/session/a/0", "/v5/session/a/1", "/v5/session/a/2"],
                    ["/v5/session/b/0", "/v5/session/b/1", "/v5/session/b/2"],
                ]
                barrier = threading.Barrier(2)
                results = [None, None]
                errors = [None, None]
                threads = [
                    threading.Thread(
                        target=https_worker,
                        args=(client_port, paths[index], barrier, results, errors, index),
                    )
                    for index in range(2)
                ]
                for thread in threads:
                    thread.start()
                for thread in threads:
                    thread.join(timeout=10.0)
                for thread in threads:
                    if thread.is_alive():
                        raise RuntimeError("HTTPS Zero-RTT multi-session worker timed out")
                for error in errors:
                    if error is not None:
                        raise error

                for index, session_paths in enumerate(paths):
                    expected = [response_body(path) for path in session_paths]
                    if results[index] != expected:
                        raise RuntimeError(
                            f"session {index} received unexpected bodies: {results[index]!r}"
                        )

                expected_paths = sorted(path for session_paths in paths for path in session_paths)
                if sorted(origin.request_paths) != expected_paths:
                    raise RuntimeError(f"unexpected origin paths: {origin.request_paths!r}")
            logs = relays.logs
        finally:
            stop_origin(origin)

    if logs.count("event=zero_rtt_authenticated") < 4:
        raise RuntimeError(f"expected both relays to authenticate two sessions:\n{logs}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
