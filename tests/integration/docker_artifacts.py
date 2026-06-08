#!/usr/bin/env python3

import argparse
import sys
from pathlib import Path


def read(path):
    return path.read_text(encoding="utf-8")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def require_all(text, expected, label):
    missing = [item for item in expected if item not in text]
    require(not missing, f"{label} missing {missing!r}")


def reject_secrets(text, label):
    forbidden = [
        "BEGIN PRIVATE KEY",
        "shared_secret",
        "secret_file",
        "client.key contents",
        "server.key contents",
        "client_private_key",
        "client_public_key",
        "server_private_key_file",
        "server_public_key_file",
        "allowed_client_public_key",
    ]
    for item in forbidden:
        require(item not in text, f"{label} appears to contain secret material marker {item!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    args = parser.parse_args()
    repo = Path(args.repo)
    removed_hold_command = "hold" + "-wss"
    removed_public_echo = "echo" + ".websocket.org"

    for dockerfile_name, label in [
        ("Dockerfile", "base Dockerfile"),
        ("Dockerfile.alpine", "Alpine Dockerfile"),
    ]:
        dockerfile = read(repo / dockerfile_name)
        reject_secrets(dockerfile, label)
        require("dante-server" not in dockerfile, f"{label} must not install Dante")
        require(
            "fps-socks-entrypoint" not in dockerfile,
            f"{label} must not install SOCKS entrypoint",
        )
        require("EXPOSE 1080/tcp" not in dockerfile, f"{label} must not expose proxy port")

    requirements = read(repo / "requirements-runtime.txt")
    require("websockets==" in requirements, "runtime requirements must pin websockets")
    reject_secrets(requirements, "runtime requirements")

    dockerignore = read(repo / ".dockerignore")
    require_all(
        dockerignore,
        [".git", ".github", "build", "cmake-build-*", "captures", "*.pcap", "__pycache__"],
        ".dockerignore",
    )

    license_text = read(repo / "LICENSE")
    require_all(
        license_text,
        ["MIT License", "Copyright (c) 2026 FPS contributors"],
        "license",
    )

    docs_index = read(repo / "docs/index.md")
    require_all(
        docs_index,
        [
            "Public beta quickstart",
            "Docker runtime",
            "Client profiles and carrier setup",
            "UUID and key rotation",
            "Release checklist",
            "Beta status",
        ],
        "docs index",
    )

    quickstart_docs = read(repo / "docs/public-beta-quickstart.md")
    require_all(
        quickstart_docs,
        [
            "examples/docker/server",
            "examples/docker/client-host",
            "fps_carrier origin",
            "fps_carrier client",
            "fps_server --generate-server-keypair --format json",
            "nc -vz fps.example.net 443",
            "Do not use",
            "--output /tmp/client.json",
            "/etc/hosts",
            "docker compose run --rm --no-deps fps-server status",
            "docker compose run --rm --no-deps fps-client status",
            "examples/docker/proxy-dante/Dockerfile",
            "socks5h://10.66.0.1:1080",
            "one UUID per client device or profile",
        ],
        "public beta quickstart",
    )
    reject_secrets(quickstart_docs, "public beta quickstart")
    require(
        removed_public_echo not in quickstart_docs,
        "public beta quickstart must not depend on public echo",
    )
    require(
        "Do not open `https://127.0.0.1/`" in quickstart_docs,
        "public beta quickstart must warn against loopback browser origin",
    )

    rotation_docs = read(repo / "docs/rotation.md")
    require_all(
        rotation_docs,
        [
            "Reissue One Client UUID",
            "--lease-revoke-client-uuid",
            "--lease-prune",
            "Rotate The Server Key Pair",
            "Validation Drill",
            "old generated client profile",
            "There is no config hot reload",
            "Do not share one UUID across devices",
            "replace_old",
        ],
        "rotation docs",
    )
    reject_secrets(rotation_docs, "rotation docs")

    release_docs = read(repo / "docs/release.md")
    require_all(
        release_docs,
        [
            "Release Checklist",
            "python3 tests/integration/docker_artifacts.py --repo /workspaces",
            "Docker Images",
            "Publish Images",
            "ghcr.io/OWNER/fps:v0.1.0-beta.1",
            "two-host soak",
            "v0.1.0-beta.1",
            "MIT",
            "Signed Docker images",
        ],
        "release docs",
    )
    reject_secrets(release_docs, "release docs")

    beta_status = read(repo / "docs/beta-status.md")
    require_all(
        beta_status,
        [
            "controlled Linux/Docker beta deployments",
            "Manual GHCR",
            "reproducible two-host",
            "current review brief",
            "classified FPS records",
            "no-timestamp/no-cache replay model",
            "Traffic-shape mimicry remains incomplete",
        ],
        "beta status docs",
    )
    reject_secrets(beta_status, "beta status docs")

    github_ops = read(repo / "dev/GITHUB_OPERATIONS.md")
    require_all(
        github_ops,
        [
            "Do not reuse old review snapshots",
            "docs/specification.md",
            "docs/testing.md",
            "docs/beta-status.md",
            "latest soak evidence",
            "shaper interaction",
        ],
        "GitHub operations docs",
    )
    reject_secrets(github_ops, "GitHub operations docs")

    entrypoint = read(repo / "docker/fps-entrypoint.sh")
    require_all(
        entrypoint,
        [
            "FPS_ROLE",
            "FPS_CONFIG",
            "FPS_LOG_LEVEL",
            "FPS_CONFIGURE_TUN",
            "FPS_TUN_NAME",
            "FPS_TUN_ADDRESS",
            "FPS_TUN_MTU",
            "FPS_TUN_ROUTES",
            "--check-config",
            "run_role_alias",
            "check-config|status",
            "--status",
            "ip addr replace",
            "ip route replace",
            'exec "$@"',
        ],
        "entrypoint",
    )
    reject_secrets(entrypoint, "entrypoint")

    ci_local = read(repo / "docker/ci-local.sh")
    require_all(
        ci_local,
        [
            "FPS_COMPILER",
            "clang++-20",
            "python3 -m py_compile",
            "import websockets",
            "bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh",
            "-DFPS_BUILD_TESTS=ON",
            "-DFPS_ENABLE_TUN_TESTS=OFF",
            "ctest --test-dir",
        ],
        "CI local runner",
    )
    reject_secrets(ci_local, "CI local runner")

    publish_workflow_path = repo / ".github/workflows/publish-images.yml"
    old_publish_workflow_path = repo / ".github/workflows/publish-private-images.yml"
    release_candidate_workflow_path = repo / ".github/workflows/release-candidate.yml"
    require(
        not old_publish_workflow_path.exists(),
        "old private image workflow path must not reappear",
    )
    require(
        not release_candidate_workflow_path.exists(),
        "release-candidate workflow is intentionally folded into Publish Images publish=false",
    )
    if publish_workflow_path.exists():
        publish_workflow = read(publish_workflow_path)
        require_all(
            publish_workflow,
            [
                "Publish Images",
                "workflow_dispatch",
                "packages: write",
                "publish:",
                "actions/checkout@v6",
                "docker/setup-buildx-action@v4",
                "docker/login-action@v4",
                "docker/metadata-action@v6",
                "docker/build-push-action@v7",
                "tools/run_quality_checks.sh --docker",
                "Dockerfile.alpine",
                "type=raw,value=${{ steps.image.outputs.version_tag }},enable=${{ matrix.variant == 'ubuntu' }}",
                "type=raw,value=${{ steps.image.outputs.version_tag }}-alpine,enable=${{ matrix.variant == 'alpine' }}",
                "provenance: false",
                "sbom: false",
                "cache-to: type=gha,mode=max",
            ],
            "image publish workflow",
        )
        reject_secrets(publish_workflow, "image publish workflow")

    android_emulator_workflow = read(repo / ".github/workflows/android-emulator.yml")
    require_all(
        android_emulator_workflow,
        [
            "Android Emulator",
            "workflow_dispatch",
            "ubuntu-24.04",
            "docker/setup-buildx-action@v4",
            "/dev/kvm",
            "tools/run_android_checks.sh --docker-managed-device",
        ],
        "Android emulator workflow",
    )
    reject_secrets(android_emulator_workflow, "Android emulator workflow")

    android_checks = read(repo / "tools/run_android_checks.sh")
    require_all(
        android_checks,
        [
            "--managed-device",
            "--docker-managed-device",
            "FPS_ANDROID_REUSE_DOCKER_IMAGE",
            "FPS_ANDROID_EMULATOR_IMAGE",
            "FPS_ANDROID_EMULATOR_DOCKERFILE",
            "FPS_ANDROID_MANAGED_DEVICE_TASK",
            "FPS_ANDROID_EMULATOR_GPU",
            "fpsApi30AtdDebugAndroidTest",
            "/dev/kvm",
            "ensure_docker_image",
        ],
        "Android check helper",
    )
    reject_secrets(android_checks, "Android check helper")

    require(
        (repo / "Dockerfile.android").exists(),
        "Android build Dockerfile must exist",
    )
    require(
        (repo / "Dockerfile.android-emulator").exists(),
        "Android emulator Dockerfile must exist",
    )

    android_gradle = read(repo / "android/app/build.gradle.kts")
    require_all(
        android_gradle,
        [
            "fpsApi30Atd",
            "apiLevel = 30",
            "systemImageSource = \"aosp-atd\"",
            "testedAbi = \"x86_64\"",
        ],
        "Android Gradle managed-device config",
    )
    reject_secrets(android_gradle, "Android Gradle managed-device config")

    quality_checks = read(repo / "tools/run_quality_checks.sh")
    require_all(
        quality_checks,
        [
            "FPS_DOCKER_BUILDKIT=0",
            "docker_build_cmd",
            "buildx build --load",
            "falling back to classic docker build",
            "FPS_DOCKERFILE",
            "fps_client --generate-client-uuid",
            "fps_carrier --help",
            "fps_server --generate-server-keypair --format json",
            "allowed_client_uuids",
            "fps_server --check-config --config /etc/fps/server.json",
            "\"$image\" check-config",
            "Docker explicit command passthrough smoke",
            "examples/docker/proxy-dante/compose.yml",
        ],
        "quality checks",
    )
    reject_secrets(quality_checks, "quality checks")
    require(
        removed_hold_command not in quality_checks,
        "quality checks must not smoke removed hold command",
    )
    require("fps-socks-entrypoint.sh --help" not in quality_checks, "Docker smoke must not require SOCKS in base image")
    require("danted -v" not in quality_checks, "Docker smoke must not require Dante in base image")

    server_compose = read(repo / "examples/docker/server/compose.yml")
    require_all(
        server_compose,
        [
            "image: fps:local",
            "context: ../../..",
            "dockerfile: ${FPS_DOCKERFILE:-Dockerfile}",
            "/dev/net/tun:/dev/net/tun",
            "NET_ADMIN",
            "NET_BIND_SERVICE",
            "no-new-privileges:true",
            "${FPS_PUBLISHED_PORT:-8443}:8443/tcp",
            "FPS_ROLE: server",
            "FPS_CONFIGURE_TUN: \"1\"",
            "FPS_TUN_NAME: fpss0",
            "fps-server-run:/run/fps",
        ],
        "server compose",
    )
    require("privileged: true" not in server_compose, "server compose must not use privileged")
    require("/etc/fps/keys" not in server_compose, "server compose must not mount key files")
    require("fps-socks" not in server_compose, "server compose must not include proxy sidecar")
    require("FPS_SOCKS_" not in server_compose, "server compose must not include proxy env")
    reject_secrets(server_compose, "server compose")

    dante_dockerfile = read(repo / "examples/docker/proxy-dante/Dockerfile")
    reject_secrets(dante_dockerfile, "Dante proxy Dockerfile")

    dante_entrypoint = read(repo / "examples/docker/proxy-dante/fps-dante-entrypoint.sh")
    require_all(
        dante_entrypoint,
        [
            "FPS_SOCKS_LISTEN_ADDRESS",
            "FPS_SOCKS_PORT",
            "FPS_SOCKS_ALLOWED_CIDR",
            "FPS_SOCKS_EXTERNAL_INTERFACE",
            "danted",
            "socksmethod: none",
            "command: connect",
        ],
        "Dante proxy entrypoint",
    )
    reject_secrets(dante_entrypoint, "Dante proxy entrypoint")

    dante_compose = read(repo / "examples/docker/proxy-dante/compose.yml")
    require_all(
        dante_compose,
        [
            "fps-dante-proxy",
            "dockerfile: examples/docker/proxy-dante/Dockerfile",
            "FPS_BASE_IMAGE",
            "network_mode: \"service:fps-server\"",
            "FPS_SOCKS_LISTEN_ADDRESS",
            "FPS_SOCKS_ALLOWED_CIDR",
            "no-new-privileges:true",
        ],
        "Dante proxy compose",
    )
    require("privileged: true" not in dante_compose, "Dante proxy compose must not use privileged")
    reject_secrets(dante_compose, "Dante proxy compose")

    client_compose = read(repo / "examples/docker/client-host/compose.yml")
    require_all(
        client_compose,
        [
            "network_mode: host",
            "dockerfile: ${FPS_DOCKERFILE:-Dockerfile}",
            "/dev/net/tun:/dev/net/tun",
            "NET_ADMIN",
            "NET_BIND_SERVICE",
            "FPS_ROLE: client",
            "FPS_CONFIGURE_TUN: \"1\"",
            "FPS_TUN_NAME: fpsc0",
            "fps-client-run:/run/fps",
            "FPS TUN routes affect the host",
        ],
        "client compose",
    )
    require("privileged: true" not in client_compose, "client compose must not use privileged")
    require("/etc/fps/keys" not in client_compose, "client compose must not mount key files")
    require("FPS_TUN_ROUTES" not in client_compose, "client compose must not set default routes")
    reject_secrets(client_compose, "client compose")

    debug_compose = read(repo / "examples/docker/debug-carrier/compose.yml")
    require_all(
        debug_compose,
        [
            "fps-carrier-origin",
            "fps-carrier-client",
            "fps_carrier",
            "dockerfile: ${FPS_DOCKERFILE:-Dockerfile}",
            "--generate-self-signed",
            "fps-client:7443",
            "fps-server-run:/run/fps",
            "fps-client-run:/run/fps",
            "no-new-privileges:true",
        ],
        "debug carrier compose",
    )
    require("privileged: true" not in debug_compose, "debug carrier compose must not use privileged")
    require("/etc/fps/keys" not in debug_compose, "debug carrier compose must not mount key files")
    reject_secrets(debug_compose, "debug carrier compose")
    debug_server_config = read(repo / "examples/docker/debug-carrier/config/server.json")
    require_all(
        debug_server_config,
        [
            "fps-carrier-origin:9443",
            "debug-carrier-v5",
            "server_private_key_base64",
            "server_public_key_base64",
            "allowed_client_uuids",
        ],
        "debug carrier server config",
    )
    reject_secrets(debug_server_config, "debug carrier server config")

    server_config = read(repo / "examples/docker/server/config/server.json")
    require_all(
        server_config,
        [
            "server_private_key_base64",
            "server_public_key_base64",
            "allowed_client_uuids",
            "lease_pool",
            "lease_file",
            "client_isolation",
            "status_socket",
        ],
        "server config",
    )
    reject_secrets(server_config, "server config")

    client_config = read(repo / "examples/docker/client-host/config/client.json")
    require_all(
        client_config,
        ["client_uuid", "server_public_key_base64", "auto_configure", "status_socket"],
        "client config",
    )
    reject_secrets(client_config, "client config")

    docs = read(repo / "docs/docker.md")
    require_all(
        docs,
        [
            "public-beta-quickstart.md",
            "docker build -t fps:local .",
            "Dockerfile.alpine",
            "Alpine",
            "FPS_DOCKERFILE=Dockerfile.alpine",
            "fps_carrier",
            "docker_tun_iperf_sim.py",
            "docker_multi_client_sim.py",
            "docker_duplicate_uuid_sim.py",
            "websockets",
            "iperf3",
            "SOCKS5",
            "Dante",
            "DHCP",
            "L3 TUN",
            "NET_ADMIN",
            "network_mode: host",
            "FPS_PUBLISHED_PORT=443",
            "nc -vz fps.example.net 443",
            "provider firewall or security group",
            "target_connect_failed",
            "no_carrier_session",
            "non_ipv4_tun_destination",
            "127.0.0.1 is container loopback",
            "--output /tmp/client.json",
            "ops.status_socket",
            "docker compose run --rm --no-deps fps-server status",
            "docker compose run --rm --no-deps fps-client status",
            "--user \"$(id -u):$(id -g)\"",
            "--status",
        ],
        "Docker docs",
    )
    reject_secrets(docs, "Docker docs")
    require(removed_hold_command not in docs, "Docker docs must not mention removed hold command")
    require(removed_public_echo not in docs, "Docker docs must not depend on public echo")
    require("fps-socks-entrypoint.sh --help" not in docs, "Docker docs must not advertise SOCKS in base image")

    client_profile_docs = read(repo / "docs/client-profiles.md")
    require_all(
        client_profile_docs,
        [
            "public-beta-quickstart.md",
            "rotation.md",
            "Do not share one `client_uuid` across multiple devices",
            "replace_old",
            "--output /tmp/client.json",
            "client_upgrade_delay_sigma_ms",
        ],
        "client profile docs",
    )
    reject_secrets(client_profile_docs, "client profile docs")

    proxy_docs = read(repo / "docs/proxy-overlays.md")
    require_all(
        proxy_docs,
        [
            "deployment overlays",
            "Dante SOCKS5",
            "examples/docker/proxy-dante/Dockerfile",
            "fps-dante-proxy",
            "socks5h://10.66.0.1:1080",
            "Squid HTTP Proxy",
            "http_port 10.66.0.1:3128",
        ],
        "proxy overlay docs",
    )
    reject_secrets(proxy_docs, "proxy overlay docs")

    multi_client_sim = read(repo / "tools/docker_multi_client_sim.py")
    require_all(
        multi_client_sim,
        [
            "fps-client-a",
            "fps-client-b",
            "allowed_client_uuids",
            "ignored_spoofed_tun_source",
            "event=session_stats",
            "iperf3",
            "SPOOF_IP",
        ],
        "Docker multi-client simulation",
    )
    reject_secrets(multi_client_sim, "Docker multi-client simulation")

    duplicate_uuid_sim = read(repo / "tools/docker_duplicate_uuid_sim.py")
    require_all(
        duplicate_uuid_sim,
        [
            "fps-client-a",
            "fps-client-b",
            "event=duplicate_client_replaced",
            "duplicate_client_replacements",
            "client_instance_id",
            "old_client_server_to_client_blocked",
            "new_client_server_to_client_ok",
        ],
        "Docker duplicate UUID simulation",
    )
    reject_secrets(duplicate_uuid_sim, "Docker duplicate UUID simulation")

    resilience_soak = read(repo / "tools/docker_resilience_soak.py")
    require_all(
        resilience_soak,
        [
            "import sys",
            "--stress-backpressure",
            "--require-backpressure-event",
            "observed_write_queue_full",
            "write_queue_full_before",
            "write_queue_full_after",
            "ignored_spoofed_tun_source",
            "socks_overlay_ok",
            "print(f\"error: {error}\", file=sys.stderr)",
            "down\", \"-v\", \"--remove-orphans",
        ],
        "Docker resilience soak",
    )
    reject_secrets(resilience_soak, "Docker resilience soak")

    two_host_soak = read(repo / "tools/docker_two_host_soak.py")
    require_all(
        two_host_soak,
        [
            "--remote",
            "--remote-connect-host",
            "--build-local",
            "--transfer-image",
            "transfer_docker_image",
            "copy_tree_to_remote",
            "remote_compose",
            "carriers-per-client",
            "--max-loss-percent",
            "BAD_LOG_PATTERNS",
            "ignored_spoofed_tun_source",
            "down\", \"-v\", \"--remove-orphans",
            "rm -rf",
            "fps-two-host-udp-client",
            "fps-two-host-http-client",
            "local-tmp.logs",
            "remote-tmp.logs",
        ],
        "Docker two-host soak",
    )
    reject_secrets(two_host_soak, "Docker two-host soak")

    return 0


if __name__ == "__main__":
    sys.exit(main())
