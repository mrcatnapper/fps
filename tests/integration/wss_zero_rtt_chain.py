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
    prepare_zero_rtt_fixture_dir,
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

        server_config = tmpdir / "server-v4.json"
        client_config = tmpdir / "client-v4.json"
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

            run_carrier_client(
                args.carrier, client_port, "/fps-carrier-zero-rtt", frames=3, seed=37
            )
        finally:
            terminate(client_proc)
            terminate(server_proc)
            terminate(origin)

    return 0


if __name__ == "__main__":
    sys.exit(main())
