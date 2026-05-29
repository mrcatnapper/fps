#!/usr/bin/env python3

import argparse
import sys
import tempfile
from pathlib import Path

from fps_https_harness import (
    assert_log_contains,
    assert_log_omits,
    assert_origin_paths,
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


LARGE_PATH = "/large/98304"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps-client", required=True)
    parser.add_argument("--fps-server", required=True)
    args = parser.parse_args()

    server_proc = None
    client_proc = None
    server_log = ""
    client_log = ""
    with tempfile.TemporaryDirectory() as tmpdir_str:
        tmpdir = Path(tmpdir_str)
        cert, key = generate_cert(tmpdir)
        prepare_zero_rtt_fixture_dir(tmpdir)

        origin_port = free_port()
        server_port = free_port()
        client_port = free_port()
        origin = start_https_origin(cert, key, origin_port)

        server_config = tmpdir / "server-v5.json"
        client_config = tmpdir / "client-v5.json"
        write_zero_rtt_relay_config(
            server_config,
            server_port,
            "origin",
            origin_port,
            "server",
            read_buffer_size=65536,
        )
        write_zero_rtt_relay_config(
            client_config,
            client_port,
            "server",
            server_port,
            "client",
            read_buffer_size=65536,
        )

        try:
            server_proc = start_process(
                [args.fps_server, "--config", str(server_config), "--log-level", "debug"]
            )
            wait_for_tcp("127.0.0.1", server_port, server_proc)

            client_proc = start_process(
                [args.fps_client, "--config", str(client_config), "--log-level", "debug"]
            )
            wait_for_tcp("127.0.0.1", client_port, client_proc)

            bodies = https_get_roundtrips(client_port, paths=[LARGE_PATH])
            assert_roundtrip_bodies(bodies, paths=[LARGE_PATH])
            assert_origin_paths(origin, paths=[LARGE_PATH])
        finally:
            client_log = stop_and_read(client_proc)
            server_log = stop_and_read(server_proc)
            stop_origin(origin)

    logs = client_log + "\n" + server_log
    assert_log_contains(logs, "event=zero_rtt_authenticated")
    for forbidden in [
        "event=envelope_error",
        "event=envelope_encode_error",
        "event=carrier_removed",
        "raw_payload",
        "session_key",
    ]:
        assert_log_omits(logs, forbidden)
    return 0


if __name__ == "__main__":
    sys.exit(main())
