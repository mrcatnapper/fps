#!/usr/bin/env python3

import argparse
import sys
import tempfile
from pathlib import Path

from fps_https_harness import (
    free_port,
    generate_cert,
    run_carrier_client,
    start_carrier_origin,
    terminate,
    prepare_zero_rtt_fixture_dir,
    ZeroRttRelayPair,
)


DEFAULT_CARRIER = str(Path(__file__).resolve().parents[2] / "tools" / "fps_carrier.py")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps-client", required=True)
    parser.add_argument("--fps-server", required=True)
    parser.add_argument("--carrier", default=DEFAULT_CARRIER)
    args = parser.parse_args()

    origin = None
    with tempfile.TemporaryDirectory() as tmpdir_str:
        tmpdir = Path(tmpdir_str)
        cert, key = generate_cert(tmpdir)
        prepare_zero_rtt_fixture_dir(tmpdir)

        origin_port = free_port()
        server_port = free_port()
        client_port = free_port()
        origin = start_carrier_origin(
            args.carrier, cert, key, origin_port, "/fps-carrier-zero-rtt"
        )

        try:
            with ZeroRttRelayPair(
                tmpdir=tmpdir,
                fps_client=args.fps_client,
                fps_server=args.fps_server,
                origin_port=origin_port,
                server_port=server_port,
                client_port=client_port,
                log_level=None,
            ):
                run_carrier_client(
                    args.carrier, client_port, "/fps-carrier-zero-rtt", frames=3, seed=37
                )
        finally:
            terminate(origin)

    return 0


if __name__ == "__main__":
    sys.exit(main())
