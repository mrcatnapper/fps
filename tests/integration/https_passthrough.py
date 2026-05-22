#!/usr/bin/env python3

import argparse
import sys
import tempfile

from fps_https_harness import (
    assert_origin_paths,
    assert_roundtrip_bodies,
    free_port,
    generate_cert,
    https_get_roundtrips,
    start_https_origin,
    start_process,
    stop_origin,
    terminate,
    wait_for_tcp,
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps-server", required=True)
    args = parser.parse_args()

    relay = None
    with tempfile.TemporaryDirectory() as tmpdir:
        cert, key = generate_cert(tmpdir)
        origin_port = free_port()
        relay_port = free_port()
        origin = start_https_origin(cert, key, origin_port)
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
            bodies = https_get_roundtrips(relay_port)
            assert_roundtrip_bodies(bodies)
            assert_origin_paths(origin)
        finally:
            terminate(relay)
            stop_origin(origin)

    return 0


if __name__ == "__main__":
    sys.exit(main())
