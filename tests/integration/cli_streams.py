#!/usr/bin/env python3

import argparse
import json
import re
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

from fps_https_harness import free_port


def run_command(args):
    return subprocess.run(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def check_help(binary, expect_lease_commands):
    completed = run_command([binary, "--help"])
    if completed.returncode != 0:
        raise RuntimeError(f"{binary} --help returned {completed.returncode}")
    required = [
        "Usage:",
        "--log-level",
        "--check-config",
        "--status",
        "--status-socket",
        "--generate-server-keypair",
        "--generate-client-uuid",
    ]
    if expect_lease_commands:
        required.extend([
            "--lease-list",
            "--lease-revoke-client-uuid",
            "--lease-prune",
            "--generate-client-profile",
            "--server-endpoint",
        ])
    else:
        required.append("--print-config-from-uri")
        required.append("--write-config-from-uri")
    if not all(text in completed.stdout for text in required):
        raise RuntimeError(f"{binary} help did not include expected usage text")
    if not expect_lease_commands and "--lease-list" in completed.stdout:
        raise RuntimeError(f"{binary} help advertised server-only lease commands")
    if expect_lease_commands and "--print-config-from-uri" in completed.stdout:
        raise RuntimeError(f"{binary} help advertised client-only URI import")
    if completed.stderr:
        raise RuntimeError(f"{binary} --help wrote to stderr: {completed.stderr!r}")


def check_invalid_log_level(binary, target_flag):
    listen_port = free_port()
    target_port = free_port()
    completed = run_command(
        [
            binary,
            "--listen",
            f"127.0.0.1:{listen_port}",
            target_flag,
            f"127.0.0.1:{target_port}",
            "--log-level",
            "verbose",
        ]
    )
    if completed.returncode != 2:
        raise RuntimeError(f"{binary} invalid --log-level returned {completed.returncode}")
    if completed.stdout:
        raise RuntimeError(f"{binary} invalid --log-level wrote stdout: {completed.stdout!r}")
    if "invalid --log-level value" not in completed.stderr:
        raise RuntimeError(f"{binary} invalid --log-level missing diagnostic")
    if "event=" in completed.stderr or " level=" in completed.stderr:
        raise RuntimeError(f"{binary} parse error included runtime logs: {completed.stderr!r}")


def expect_clean_success(completed, context):
    if completed.returncode != 0:
        raise RuntimeError(
            f"{context} returned {completed.returncode}, stderr={completed.stderr!r}"
        )
    if completed.stderr:
        raise RuntimeError(f"{context} wrote stderr: {completed.stderr!r}")
    if "event=" in completed.stdout or " level=" in completed.stdout:
        raise RuntimeError(f"{context} stdout included runtime logs: {completed.stdout!r}")


def check_key_tooling(binary):
    completed = run_command([binary, "--generate-server-keypair"])
    expect_clean_success(completed, f"{binary} --generate-server-keypair")
    for pattern in [
        r"server_private_key_base64=[A-Za-z0-9+/]{43}=\n",
        r"server_public_key_base64=[A-Za-z0-9+/]{43}=\n",
    ]:
        if re.search(pattern, completed.stdout) is None:
            raise RuntimeError(f"server key helper output is incomplete: {completed.stdout!r}")

    completed = run_command([binary, "--generate-client-uuid"])
    expect_clean_success(completed, f"{binary} --generate-client-uuid")
    if re.fullmatch(
        r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\n",
        completed.stdout,
    ) is None:
        raise RuntimeError(f"generate-client-uuid output is not canonical: {completed.stdout!r}")

    raw_removed = run_command([binary, "--generate-client-uuid", "--raw"])
    if raw_removed.returncode != 2:
        raise RuntimeError(f"--generate-client-uuid --raw returned {raw_removed.returncode}")
    if raw_removed.stdout:
        raise RuntimeError(f"--raw wrote stdout: {raw_removed.stdout!r}")
    if "unknown option" not in raw_removed.stderr:
        raise RuntimeError(f"--raw diagnostic missing unknown option: {raw_removed.stderr!r}")

    for unknown in [["--definitely-unknown"], ["--another-unknown", "value"]]:
        completed = run_command([binary, *unknown])
        if completed.returncode != 2:
            raise RuntimeError(f"{binary} {' '.join(unknown)} returned {completed.returncode}")
        if completed.stdout:
            raise RuntimeError(f"unknown option wrote stdout: {completed.stdout!r}")
        if "unknown option" not in completed.stderr:
            raise RuntimeError(f"unknown option missing diagnostic: {completed.stderr!r}")


def check_config_summary(binary, target_field, target_flag):
    with tempfile.TemporaryDirectory(prefix="fps-cli-config-") as temp_name:
        temp = Path(temp_name)
        config_path = temp / "relay.json"
        config = {
            "network": {
                "listen": f"127.0.0.1:{free_port()}",
                target_field: f"127.0.0.1:{free_port()}",
            },
            "logging": {"level": "debug"},
        }
        config_path.write_text(json.dumps(config), encoding="utf-8")

        completed = run_command(
            [binary, "--check-config", "--config", str(config_path)]
        )
        expect_clean_success(completed, f"{binary} --check-config")
        required = [
            "config=valid",
            "log_level=debug",
            "zero_rtt_enabled=0",
            f"{target_field}=127.0.0.1:",
        ]
        if not all(text in completed.stdout for text in required):
            raise RuntimeError(f"check-config summary is incomplete: {completed.stdout!r}")

        completed = run_command(
            [
                binary,
                "--check-config",
                "--config",
                str(config_path),
                "--log-level",
                "error",
            ]
        )
        expect_clean_success(completed, f"{binary} --check-config --log-level")
        if "log_level=error" not in completed.stdout:
            raise RuntimeError(
                f"check-config did not apply CLI log override: {completed.stdout!r}"
            )

        completed = run_command([binary, target_flag, "127.0.0.1:1"])
        if completed.returncode != 2:
            raise RuntimeError(f"{binary} incomplete run command returned {completed.returncode}")


def parse_server_keypair(text):
    values = {}
    for line in text.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    for key in ["server_private_key_base64", "server_public_key_base64"]:
        if key not in values:
            raise RuntimeError(f"missing {key} in server keypair output: {text!r}")
    return values


def check_client_profile_generation(fps_server, fps_client):
    client_uuid = "123e4567-e89b-42d3-a456-426614174000"
    keypair = run_command([fps_server, "--generate-server-keypair"])
    expect_clean_success(keypair, f"{fps_server} --generate-server-keypair")
    keys = parse_server_keypair(keypair.stdout)

    with tempfile.TemporaryDirectory(prefix="fps-cli-profile-") as temp_name:
        temp = Path(temp_name)
        server_config = temp / "server.json"
        server_config.write_text(
            json.dumps(
                {
                    "network": {
                        "listen": f"127.0.0.1:{free_port()}",
                        "origin": f"127.0.0.1:{free_port()}",
                    },
                    "security": {
                        "zero_rtt": {
                            "enabled": True,
                            "profile_id": "cli-profile-v3",
                            "server_private_key_base64": keys[
                                "server_private_key_base64"
                            ],
                            "server_public_key_base64": keys[
                                "server_public_key_base64"
                            ],
                            "allowed_client_uuids": [client_uuid],
                        }
                    },
                    "codec": {
                        "max_frame_payload": 1280,
                        "max_frame_padding": 64,
                        "allow_fragmentation": True,
                    },
                    "tun": {
                        "enabled": True,
                        "name": "fpss0",
                        "mtu": 1280,
                        "lease_pool": "10.77.0.0/29",
                        "server_address": "10.77.0.1",
                        "lease_file": "leases.json",
                    },
                }
            ),
            encoding="utf-8",
        )

        completed = run_command(
            [
                fps_server,
                "--generate-client-profile",
                "--config",
                str(server_config),
                "--client-uuid",
                client_uuid,
                "--server-endpoint",
                "vpn.example.test:8443",
                "--client-listen",
                "127.0.0.1:17443",
                "--client-tun",
                "fpscli0",
                "--client-status-socket",
                "/run/fps/client.status",
            ]
        )
        expect_clean_success(completed, f"{fps_server} --generate-client-profile")
        if keys["server_private_key_base64"] in completed.stdout:
            raise RuntimeError("generated client profile leaked server private key")
        if "allowed_client_uuids" in completed.stdout:
            raise RuntimeError("generated client profile leaked server allowlist")
        profile = json.loads(completed.stdout)
        if profile["network"]["server"] != "vpn.example.test:8443":
            raise RuntimeError(f"generated profile has wrong endpoint: {profile!r}")
        if profile["security"]["zero_rtt"]["client_uuid"] != client_uuid:
            raise RuntimeError("generated profile lost client UUID")
        if profile["security"]["zero_rtt"]["server_public_key_base64"] != keys[
            "server_public_key_base64"
        ]:
            raise RuntimeError("generated profile has wrong server public key")
        if profile["tun"]["name"] != "fpscli0" or not profile["tun"]["auto_configure"]:
            raise RuntimeError(f"generated profile has wrong TUN settings: {profile!r}")
        if profile["ops"]["status_socket"] != "/run/fps/client.status":
            raise RuntimeError(f"generated profile has wrong status socket: {profile!r}")

        output_profile = temp / "issued-client.json"
        output_generated = run_command(
            [
                fps_server,
                "--generate-client-profile",
                "--config",
                str(server_config),
                "--client-uuid",
                client_uuid,
                "--server-endpoint",
                "vpn.example.test:8443",
                "--output",
                str(output_profile),
            ]
        )
        expect_clean_success(
            output_generated,
            f"{fps_server} --generate-client-profile --output",
        )
        if output_generated.stdout:
            raise RuntimeError("--output profile generation unexpectedly wrote stdout")
        output_mode = stat.S_IMODE(output_profile.stat().st_mode)
        if output_mode != 0o600:
            raise RuntimeError(f"profile output mode is {oct(output_mode)}, expected 0o600")
        if json.loads(output_profile.read_text(encoding="utf-8"))["security"]["zero_rtt"][
            "client_uuid"
        ] != client_uuid:
            raise RuntimeError("profile output file is not the generated JSON profile")

        output_exists = run_command(
            [
                fps_server,
                "--generate-client-profile",
                "--config",
                str(server_config),
                "--client-uuid",
                client_uuid,
                "--server-endpoint",
                "vpn.example.test:8443",
                "--output",
                str(output_profile),
            ]
        )
        if output_exists.returncode != 2 or "already exists" not in output_exists.stderr:
            raise RuntimeError(
                f"profile overwrite without --force was not rejected: {output_exists!r}"
            )

        output_force = run_command(
            [
                fps_server,
                "--generate-client-profile",
                "--config",
                str(server_config),
                "--client-uuid",
                client_uuid,
                "--server-endpoint",
                "vpn.example.test:8443",
                "--format",
                "uri",
                "--output",
                str(output_profile),
                "--force",
            ]
        )
        expect_clean_success(
            output_force,
            f"{fps_server} --generate-client-profile --output --force",
        )
        if not output_profile.read_text(encoding="utf-8").startswith("fps://v1/"):
            raise RuntimeError("--force did not rewrite profile output as URI")

        client_config = temp / "client.json"
        client_config.write_text(completed.stdout, encoding="utf-8")
        checked = run_command([fps_client, "--check-config", "--config", str(client_config)])
        expect_clean_success(checked, f"{fps_client} --check-config generated profile")
        for text in ["config=valid", "zero_rtt_client_uuid_mode=1", "tun_auto_configure=1"]:
            if text not in checked.stdout:
                raise RuntimeError(
                    f"generated client profile summary missing {text}: {checked.stdout!r}"
                )

        rejected = run_command(
            [
                fps_server,
                "--generate-client-profile",
                "--config",
                str(server_config),
                "--client-uuid",
                "223e4567-e89b-42d3-a456-426614174000",
                "--server-endpoint",
                "vpn.example.test:8443",
            ]
        )
        if rejected.returncode != 2:
            raise RuntimeError(
                f"unknown client profile generation returned {rejected.returncode}"
            )
        if "not present in allowed_client_uuids" not in rejected.stderr:
            raise RuntimeError(
                f"unknown client profile generation diagnostic is wrong: {rejected.stderr!r}"
            )

        uri = run_command(
            [
                fps_server,
                "--generate-client-profile",
                "--config",
                str(server_config),
                "--client-uuid",
                client_uuid,
                "--server-endpoint",
                "vpn.example.test:8443",
                "--client-listen",
                "127.0.0.1:17443",
                "--client-tun",
                "fpscli0",
                "--client-status-socket",
                "/run/fps/client.status",
                "--format",
                "uri",
            ]
        )
        expect_clean_success(uri, f"{fps_server} --generate-client-profile --format uri")
        if not uri.stdout.startswith("fps://v1/"):
            raise RuntimeError(f"generated profile URI has wrong scheme: {uri.stdout!r}")
        if keys["server_private_key_base64"] in uri.stdout:
            raise RuntimeError("generated profile URI leaked server private key")

        decoded = run_command([fps_client, "--print-config-from-uri", uri.stdout.strip()])
        expect_clean_success(decoded, f"{fps_client} --print-config-from-uri")
        decoded_profile = json.loads(decoded.stdout)
        if decoded_profile != profile:
            raise RuntimeError("decoded profile URI did not roundtrip generated JSON profile")
        uri_config = temp / "client-uri.json"
        uri_config.write_text(decoded.stdout, encoding="utf-8")
        checked_uri = run_command([fps_client, "--check-config", "--config", str(uri_config)])
        expect_clean_success(checked_uri, f"{fps_client} --check-config URI profile")

        written_config = temp / "client-written.json"
        write_uri = run_command(
            [
                fps_client,
                "--write-config-from-uri",
                uri.stdout.strip(),
                "--output",
                str(written_config),
            ]
        )
        expect_clean_success(write_uri, f"{fps_client} --write-config-from-uri")
        if write_uri.stdout:
            raise RuntimeError("--write-config-from-uri unexpectedly wrote stdout")
        if json.loads(written_config.read_text(encoding="utf-8")) != decoded_profile:
            raise RuntimeError("written URI config does not match decoded profile")
        written_mode = stat.S_IMODE(written_config.stat().st_mode)
        if written_mode != 0o600:
            raise RuntimeError(f"written config mode is {oct(written_mode)}, expected 0o600")

        write_exists = run_command(
            [
                fps_client,
                "--write-config-from-uri",
                uri.stdout.strip(),
                "--output",
                str(written_config),
            ]
        )
        if write_exists.returncode != 2 or "already exists" not in write_exists.stderr:
            raise RuntimeError(
                f"URI import overwrite without --force was not rejected: {write_exists!r}"
            )

        write_force = run_command(
            [
                fps_client,
                "--write-config-from-uri",
                uri.stdout.strip(),
                "--output",
                str(written_config),
                "--force",
            ]
        )
        expect_clean_success(write_force, f"{fps_client} --write-config-from-uri --force")

        bad_uri = run_command([fps_client, "--print-config-from-uri", "fps://v1/not@base64"])
        if bad_uri.returncode != 2:
            raise RuntimeError(f"invalid profile URI returned {bad_uri.returncode}")
        if "invalid --print-config-from-uri value" not in bad_uri.stderr:
            raise RuntimeError(f"invalid profile URI diagnostic is wrong: {bad_uri.stderr!r}")

        bad_write_uri = run_command(
            [fps_client, "--write-config-from-uri", "fps://v1/not@base64", "--output", str(temp / "bad.json")]
        )
        if bad_write_uri.returncode != 2:
            raise RuntimeError(f"invalid write profile URI returned {bad_write_uri.returncode}")
        if "invalid --write-config-from-uri value" not in bad_write_uri.stderr:
            raise RuntimeError(
                f"invalid write profile URI diagnostic is wrong: {bad_write_uri.stderr!r}"
            )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps-client", required=True)
    parser.add_argument("--fps-server", required=True)
    args = parser.parse_args()

    check_help(args.fps_client, expect_lease_commands=False)
    check_help(args.fps_server, expect_lease_commands=True)
    check_invalid_log_level(args.fps_client, "--server")
    check_invalid_log_level(args.fps_server, "--origin")
    check_key_tooling(args.fps_client)
    check_config_summary(args.fps_client, "server", "--server")
    check_config_summary(args.fps_server, "origin", "--origin")
    check_client_profile_generation(args.fps_server, args.fps_client)
    return 0


if __name__ == "__main__":
    sys.exit(main())
