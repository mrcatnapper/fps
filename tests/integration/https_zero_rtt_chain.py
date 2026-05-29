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
    stop_origin,
    prepare_zero_rtt_fixture_dir,
    ZeroRttRelayPair,
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps-client", required=True)
    parser.add_argument("--fps-server", required=True)
    parser.add_argument("--decoy-allowed-clients", type=int, default=0)
    parser.add_argument("--origin-port", type=int)
    parser.add_argument("--server-port", type=int)
    parser.add_argument("--client-port", type=int)
    args = parser.parse_args()

    logs = ""
    with tempfile.TemporaryDirectory() as tmpdir_str:
        tmpdir = Path(tmpdir_str)
        cert, key = generate_cert(tmpdir)
        prepare_zero_rtt_fixture_dir(tmpdir)

        origin_port = args.origin_port or free_port()
        server_port = args.server_port or free_port()
        client_port = args.client_port or free_port()
        origin = start_https_origin(cert, key, origin_port)

        try:
            with ZeroRttRelayPair(
                tmpdir=tmpdir,
                fps_client=args.fps_client,
                fps_server=args.fps_server,
                origin_port=origin_port,
                server_port=server_port,
                client_port=client_port,
                decoy_allowed_clients=args.decoy_allowed_clients,
            ) as relays:
                bodies = https_get_roundtrips(client_port)
                assert_roundtrip_bodies(bodies)
                assert_origin_paths(origin)
            logs = relays.logs
        finally:
            stop_origin(origin)

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
