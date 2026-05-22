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
    start_process,
    terminate,
    wait_for_tcp,
    write_zero_rtt_relay_config,
)


DEFAULT_CARRIER = str(Path(__file__).resolve().parents[2] / "tools" / "fps_carrier.py")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps-client", required=True)
    parser.add_argument("--fps-server", required=True)
    parser.add_argument("--carrier", default=DEFAULT_CARRIER)
    args = parser.parse_args()

    server_proc = None
    client_proc = None
    origin = None
    with tempfile.TemporaryDirectory(prefix="fps-carrier-http-") as tmpdir_str:
        tmpdir = Path(tmpdir_str)
        cert, key = generate_cert(tmpdir)
        prepare_zero_rtt_fixture_dir(tmpdir)

        origin_port = free_port()
        server_port = free_port()
        client_port = free_port()
        origin = start_carrier_origin(args.carrier, cert, key, origin_port, "/fps-carrier")

        server_config = tmpdir / "server-v2.json"
        client_config = tmpdir / "client-v2.json"
        write_zero_rtt_relay_config(
            server_config, server_port, "origin", origin_port, "server"
        )
        write_zero_rtt_relay_config(
            client_config, client_port, "server", server_port, "client"
        )

        try:
            server_proc = start_process([args.fps_server, "--config", str(server_config)])
            wait_for_tcp("127.0.0.1", server_port, server_proc)

            client_proc = start_process([args.fps_client, "--config", str(client_config)])
            wait_for_tcp("127.0.0.1", client_port, client_proc)

            bodies = https_get_roundtrips(client_port, paths=["/"])
            if bodies != [b"fps_carrier echo origin path=/\n"]:
                raise RuntimeError(f"unexpected carrier HTTPS bodies: {bodies!r}")
        finally:
            terminate(client_proc)
            terminate(server_proc)
            terminate(origin)

    return 0


if __name__ == "__main__":
    sys.exit(main())
