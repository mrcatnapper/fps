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
    start_process,
    terminate,
    wait_for_tcp,
)


DEFAULT_CARRIER = str(Path(__file__).resolve().parents[2] / "tools" / "fps_carrier.py")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps-server", required=True)
    parser.add_argument("--carrier", default=DEFAULT_CARRIER)
    args = parser.parse_args()

    relay = None
    origin = None
    with tempfile.TemporaryDirectory() as tmpdir:
        cert, key = generate_cert(tmpdir)
        origin_port = free_port()
        relay_port = free_port()
        origin = start_carrier_origin(
            args.carrier, cert, key, origin_port, "/fps-carrier-passthrough"
        )
        try:
            relay = start_process(
                [
                    args.fps_server,
                    "--listen",
                    f"127.0.0.1:{relay_port}",
                    "--origin",
                    f"127.0.0.1:{origin_port}",
                ]
            )
            wait_for_tcp("127.0.0.1", relay_port, relay)
            run_carrier_client(
                args.carrier, relay_port, "/fps-carrier-passthrough", frames=3, seed=31
            )
        finally:
            terminate(relay)
            terminate(origin)

    return 0


if __name__ == "__main__":
    sys.exit(main())
