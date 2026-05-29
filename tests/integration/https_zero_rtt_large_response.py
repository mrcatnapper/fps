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
    stop_origin,
    ZeroRttRelayPair,
)


LARGE_PATH = "/large/98304"


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
                read_buffer_size=65536,
            ) as relays:
                bodies = https_get_roundtrips(client_port, paths=[LARGE_PATH])
                assert_roundtrip_bodies(bodies, paths=[LARGE_PATH])
                assert_origin_paths(origin, paths=[LARGE_PATH])
            logs = relays.logs
        finally:
            stop_origin(origin)

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
