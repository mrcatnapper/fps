#!/usr/bin/env python3

import argparse
import sys
import tempfile
from pathlib import Path

from fps_https_harness import (
    free_port,
    generate_cert,
    https_get_roundtrips,
    prepare_zero_rtt_fixture_dir,
    start_carrier_origin,
    terminate,
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
    with tempfile.TemporaryDirectory(prefix="fps-carrier-http-") as tmpdir_str:
        tmpdir = Path(tmpdir_str)
        cert, key = generate_cert(tmpdir)
        prepare_zero_rtt_fixture_dir(tmpdir)

        origin_port = free_port()
        server_port = free_port()
        client_port = free_port()
        origin = start_carrier_origin(args.carrier, cert, key, origin_port, "/fps-carrier")

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
                bodies = https_get_roundtrips(client_port, paths=["/"])
                if bodies != [b"fps_carrier echo origin path=/\n"]:
                    raise RuntimeError(f"unexpected carrier HTTPS bodies: {bodies!r}")
        finally:
            terminate(origin)

    return 0


if __name__ == "__main__":
    sys.exit(main())
