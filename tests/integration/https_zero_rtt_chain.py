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
    start_https_origin,
    start_process,
    stop_and_read,
    stop_origin,
    wait_for_tcp,
    prepare_zero_rtt_fixture_dir,
    write_zero_rtt_relay_config,
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps-client", required=True)
    parser.add_argument("--fps-server", required=True)
    parser.add_argument("--decoy-allowed-clients", type=int, default=0)
    parser.add_argument("--trial-decrypt-limit", type=int, default=8)
    parser.add_argument("--origin-port", type=int)
    parser.add_argument("--server-port", type=int)
    parser.add_argument("--client-port", type=int)
    args = parser.parse_args()

    server_proc = None
    client_proc = None
    server_log = ""
    client_log = ""
    with tempfile.TemporaryDirectory() as tmpdir_str:
        tmpdir = Path(tmpdir_str)
        cert, key = generate_cert(tmpdir)
        prepare_zero_rtt_fixture_dir(tmpdir)

        origin_port = args.origin_port or free_port()
        server_port = args.server_port or free_port()
        client_port = args.client_port or free_port()
        origin = start_https_origin(cert, key, origin_port)

        server_config = tmpdir / "server-v2.json"
        client_config = tmpdir / "client-v2.json"
        write_zero_rtt_relay_config(
            server_config,
            server_port,
            "origin",
            origin_port,
            "server",
            decoy_allowed_clients=args.decoy_allowed_clients,
            trial_decrypt_limit=args.trial_decrypt_limit,
        )
        write_zero_rtt_relay_config(
            client_config, client_port, "server", server_port, "client"
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

            bodies = https_get_roundtrips(client_port)
            assert_roundtrip_bodies(bodies)
            assert_origin_paths(origin)
        finally:
            client_log = stop_and_read(client_proc)
            server_log = stop_and_read(server_proc)
            stop_origin(origin)

    logs = client_log + "\n" + server_log
    assert_log_contains(logs, "event=zero_rtt_authenticated")

    for forbidden in [
        "event=carrier_registered",
        "raw_payload",
        "session_key",
        "fps-response:",
        "292a2b2c2d2e2f303132333435363738",
        "65666768696a6b6c6d6e6f7071727374",
    ]:
        assert_log_omits(logs, forbidden)

    return 0


if __name__ == "__main__":
    sys.exit(main())
