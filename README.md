# FPS / Free Porn Storage

FPS is an experimental Linux-first covert transport framework. Its core carries
best-effort opaque datagrams inside live TLS cover sessions; the current primary
product adapter maps those datagrams to a leased L3 TUN VPN service. Carrier
authentication uses transcript-bound Zero-RTT, visible FPS traffic is shaped as
TLS Application Data records, and post-auth covert records are inserted between
ordinary carrier TLS records.

The expanded name, **Free Porn Storage**, is intentionally misleading. It gives
the project a noisy, hard-to-search label: someone who does not already know
what FPS is looking for should not learn much from the name alone.

This repository is still beta-candidate software. It is suitable for controlled
Linux/Docker beta testing and lab deployments, not yet for unattended public
production.

## Documentation

User and operator documentation lives in [`docs/`](./docs/):

- [Documentation index](./docs/index.md)
- [Public beta quickstart](./docs/public-beta-quickstart.md)
- [Protocol and architecture specification](./docs/specification.md)
- [Docker runtime guide](./docs/docker.md)
- [Client profiles and carrier setup](./docs/client-profiles.md)
- [Proxy overlays over FPS](./docs/proxy-overlays.md)
- [Linux routing and DNS workflow](./docs/linux-routing.md)
- [UUID and key rotation](./docs/rotation.md)
- [Release checklist](./docs/release.md)
- [Testing and quality workflow](./docs/testing.md)
- [Beta status](./docs/beta-status.md)

Developer and agent working notes live in [`dev/`](./dev/). The root
[`AGENTS.md`](./AGENTS.md) remains the coordination contract for Codex/agents
working in this repository.

## Quick Start

```sh
cmake -S . -B build
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

For a Docker-first runtime path:

```sh
docker build -t fps:local .
docker run --rm fps:local fps_server --help
docker run --rm fps:local fps_client --help
```

For the end-to-end beta operator flow, use the
[public beta quickstart](./docs/public-beta-quickstart.md). It covers server
key generation, one UUID per client profile, server/client compose, `/etc/hosts`
carrier mapping, status checks and optional Dante proxy overlay.

For the full local quality set:

```sh
tools/run_quality_checks.sh --all
```

See [docs/docker.md](./docs/docker.md), [docs/client-profiles.md](./docs/client-profiles.md)
and [docs/testing.md](./docs/testing.md) for detailed operator and regression
workflows.

FPS is distributed under the [MIT license](./LICENSE).
