#!/usr/bin/env python3

import argparse
import subprocess
import sys


def run_script(script, args):
    completed = subprocess.run(
        [script, *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"{script} {' '.join(args)} returned {completed.returncode}: "
            f"stderr={completed.stderr!r}"
        )
    if completed.stderr:
        raise RuntimeError(f"{script} wrote stderr: {completed.stderr!r}")
    return completed.stdout


def require_all(output, expected):
    missing = [item for item in expected if item not in output]
    if missing:
        raise RuntimeError(f"route helper output missing {missing!r}: {output!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", required=True)
    args = parser.parse_args()

    split = run_script(
        args.script,
        [
            "plan",
            "--tun",
            "fpsc0",
            "--tun-address",
            "10.66.0.2/30",
            "--mtu",
            "1280",
            "--route",
            "10.66.1.0/24",
            "--dns",
            "10.66.0.1",
            "--dns-domain",
            "~fps.test",
        ],
    )
    require_all(
        split,
        [
            "# fps_linux_route action=plan tun=fpsc0 execute=false",
            "+ ip addr replace 10.66.0.2/30 dev fpsc0",
            "+ ip link set dev fpsc0 mtu 1280",
            "+ ip route replace 10.66.1.0/24 dev fpsc0",
            "+ resolvectl dns fpsc0 10.66.0.1",
            "+ resolvectl domain fpsc0 \\~fps.test",
        ],
    )

    full = run_script(
        args.script,
        [
            "plan",
            "--tun",
            "fpsc0",
            "--full-tunnel",
            "--table",
            "100",
            "--priority",
            "10000",
            "--fwmark",
            "0x465053",
            "--bypass",
            "203.0.113.10/32,192.0.2.1,eth0",
        ],
    )
    require_all(
        full,
        [
            "+ ip route replace 203.0.113.10/32 via 192.0.2.1 dev eth0 table 100",
            "+ ip route replace default dev fpsc0 table 100",
            "+ ip rule del priority 10000 fwmark 0x465053 table 100",
            "+ ip rule add priority 10000 fwmark 0x465053 table 100",
            "+ ip route flush cache",
        ],
    )

    cleanup = run_script(
        args.script,
        [
            "cleanup",
            "--dry-run",
            "--tun",
            "fpsc0",
            "--tun-address",
            "10.66.0.2/30",
            "--route",
            "10.66.1.0/24",
            "--full-tunnel",
            "--fwmark",
            "0x465053",
            "--bypass",
            "203.0.113.10/32,192.0.2.1,eth0",
            "--down",
        ],
    )
    require_all(
        cleanup,
        [
            "# fps_linux_route action=cleanup tun=fpsc0 execute=false",
            "+ resolvectl revert fpsc0",
            "+ ip route del 10.66.1.0/24 dev fpsc0",
            "+ ip rule del priority 10000 fwmark 0x465053 table 100",
            "+ ip route del default dev fpsc0 table 100",
            "+ ip addr del 10.66.0.2/30 dev fpsc0",
            "+ ip link set dev fpsc0 down",
        ],
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
