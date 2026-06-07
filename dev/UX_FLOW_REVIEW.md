# UX Flow Review

Date: 2026-06-03

Scope: Docker-first personal deployment flow using `fpshop` as the remote
`fps_server` host, one Linux client, self-hosted `fps_carrier` as deterministic
carrier origin, and Dante SOCKS5 overlay for application traffic. Temporary
keys, UUIDs, configs and containers were removed after the run.

This is a historical UX-flow snapshot. It is kept as an operator-test record;
current user-facing instructions live in `docs/public-beta-quickstart.md`,
`docs/docker.md`, `docs/client-profiles.md` and related docs.

## Verdict

The beta user flow is functionally viable for a personal SOCKS-over-FPS setup:

- local Alpine runtime image and derivative Dante overlay were built locally and
  loaded onto `fpshop`;
- remote `fps_server` published on TCP `:443`;
- local `fps_client` ran in host-network Docker mode and received the
  server-assigned TUN lease `10.88.0.2`;
- one carrier authenticated successfully and stayed alive;
- `www.wikipedia.org` was reached through `socks5h://10.88.0.1:1080` over the
  FPS TUN path, returning `HTTP/1.1 200 OK`;
- repeated HTTPS HEAD probes also returned `HTTP/1.1 200 OK`;
- status counters showed non-zero classified-record and TUN traffic, with no
  decode/encode/tamper failures.

The current UX is still too easy to misconfigure manually. The biggest issues
are not protocol failures; they are operator-footguns around Docker output
paths, shell parsing, public port checks and endpoint topology.

Current follow-up status:

- P0/P1 documentation and CLI fixes are addressed in current docs and CLI:
  JSON server keypair output, host-visible Docker profile generation examples,
  public `:443` preflight and host-vs-container loopback troubleshooting.
- Debug-carrier healthchecks remain optional future work; reconnect behavior is
  already covered by smoke and soak tooling.

## Flow Tested

1. Built locally:
   - `fps:uxflow-2305584` from `Dockerfile.alpine`;
   - `fps-dante-proxy:uxflow-2305584` from
     `examples/docker/proxy-dante/Dockerfile`.
2. Loaded both images to `fpshop` with `docker save | ssh fpshop docker load`.
3. Generated a temporary server keypair and one client UUID.
4. Started remote compose:
   - `fps-carrier-origin`;
   - `fps-server`;
   - `fps-dante-proxy`.
5. Started local compose:
   - `fps-client` in `network_mode: host`;
   - `fps_carrier client` connecting to `127.0.0.1:7443`.
6. Queried status on both sides.
7. Performed HTTPS over SOCKS through FPS to `www.wikipedia.org`.
8. Removed containers, volumes, remote temp directory and local secret temp
   files.

## Confirmed Good Behavior

- `fps_server --check-config` and `fps_client --check-config` gave useful
  non-secret summaries before daemon start.
- The hoster firewall only allowed the expected public HTTPS port. Publishing
  FPS on `:443` worked.
- Zero-RTT authenticated after the configured delay and assigned the client
  lease.
- The Dante overlay worked as intended when bound to server TUN address
  `10.88.0.1`.
- SOCKS remote DNS mode was sufficient for a portal-style test without changing
  the client host default route.
- `docker compose ... status` snapshots were useful and did not expose UUIDs,
  private keys or payload bytes.
- Cleanup removed the Docker containers/volumes and the `fpsc0` TUN interface.

## Issues And Friction

### P0: Docker Profile Output Footgun

The quickstart pattern that combines `docker run --rm` with
`--output /tmp/client.json` is dangerous: the file is written inside the
ephemeral container and disappears when the container exits unless `/tmp` or the
chosen output directory is bind-mounted.

Observed during the flow:

- `fps_server --generate-client-profile --output /tmp/generated-client.json`
  succeeded inside the one-shot container;
- the host then had no `/tmp/generated-client.json`.

Recommended fix:

- Update docs to prefer stdout redirection from the host shell for one-shot
  Docker profile generation, or bind-mount a writable output directory.
- Consider adding a CLI warning when `--output` points to a path that is likely
  container-local in Docker examples, or add an explicit `--format json > file`
  recipe everywhere.
- Keep generated file mode guidance explicit: stdout redirection creates host
  files with shell/umask permissions, while `--output` can enforce `0600` only
  when the path is actually on the intended filesystem.

### P0: Base64 Keypair Output Is Easy To Parse Wrong

`fps_server --generate-server-keypair` prints lines like:

```text
server_private_key_base64=...
server_public_key_base64=...
```

The base64 values are padded and can contain `=`. A naive shell parser using
`awk -F=` or `cut -d= -f2` strips padding and produces invalid config.

Observed failure:

```text
security.zero_rtt.server_private_key_base64: base64 value must be padded RFC4648 text
```

Recommended fix:

- Add `--format json` for keypair generation and use that in Docker/operator
  docs.
- If keeping text mode, document shell-safe extraction using split-on-first
  semantics, for example `cut -d= -f2-`.

### P0: Public Port Preflight Needs To Be First-Class

`fpshop` provider firewall allowed SSH and HTTPS `:443`, but blocked arbitrary
high ports. Publishing Docker port `18443:8443` looked correct on the remote
host (`docker-proxy` was listening), but external clients timed out.

Recommended fix:

- Add a quick public-port preflight to the deployment docs:
  - from server: `ss -ltnp | grep ':443'`;
  - from client: `nc -vz SERVER 443`;
  - if using a non-443 test port, verify provider firewall/security group first.
- For beta defaults, keep public examples biased toward `:443`.

### P1: Docker Network Topology Makes `127.0.0.1` Ambiguous

When local `fps_client` runs in Docker bridge mode, `network.server =
127.0.0.1:PORT` points to the container loopback, not the host loopback. This
caused repeated `target_connect_failed error=Connection refused` while an SSH
port-forward existed on the host.

The documented host-network client topology avoided the issue.

Recommended fix:

- Keep host-network compose as the primary client Docker path.
- Add a troubleshooting note: if `target_connect_failed` shows `127.0.0.1` from
  inside a container, verify whether the endpoint is meant to be host loopback,
  container loopback or a real remote address.
- For bridge-mode lab setups, document `host.docker.internal`/gateway patterns
  separately instead of mixing them into the main path.

### P1: One-Shot Compose Commands Mutate Docker State

`docker compose run --rm --no-deps fps-server check-config` created the compose
network and named volumes before the daemon was started. This is normal Docker
behavior, but it makes a "validate only" step feel less read-only.

Recommended fix:

- Prefer `docker run --rm -v "$PWD/config:/etc/fps:ro" ... check-config` in
  docs when the operator wants a strictly disposable validation command.
- Keep compose aliases for querying already-running daemons.

### P1: Generated Profile Defaults Differ From Handwritten Test Defaults

Generated client profile validation worked when stdout was redirected to a host
file. It produced a valid config, but also applied the current default
`client_upgrade_delay_sigma_ms=666` for a 2000 ms delay. That is fine for normal
runtime, but experiments and packet captures often require deterministic delay.

Recommended fix:

- Document the field in client-profile guidance and explicitly say to set sigma
  to `0` for reproducible captures/tests.

### P2: Startup Races Produce Benign But Distracting Logs

On remote start, the first inbound session hit `target_connect_failed` because
the carrier origin was not ready yet. The system recovered on the next carrier
attempt, but the first warning is distracting in a manual flow.

Recommended fix:

- Add healthchecks to debug-carrier compose examples, or make `fps_carrier`
  origin readiness easier for Compose to observe.
- Keep the existing reconnect behavior; this is a UX/log issue, not a protocol
  failure.

### P2: Expected TUN Noise Still Requires Interpretation

Server status/logs showed expected `non_ipv4_tun_destination` and
`ignored_non_ipv4_tun_packet` counters caused by host/container network noise.
The rate-limited logs are tolerable, but an operator needs to know which
counters are benign in a minimal deployment.

Recommended fix:

- Add a short "expected startup/status noise" table to Docker troubleshooting.

### P2: Docker Legacy Builder Warning

Local `docker build` emitted the Docker legacy builder deprecation warning.
This does not affect correctness, but it distracts from operator output.

Recommended fix:

- Decide whether to document BuildKit/buildx as the preferred local build path,
  or leave this as environment-specific noise.

## Current Status Of Suggested Fixes

The original P0/P1 follow-ups from this run are complete in current docs and
tooling:

- profile generation examples now write to the host filesystem intentionally or
  document bind-mounted output paths;
- server keypair generation supports JSON output and docs prefer it for scripts;
- deployment docs include public `:443` and host-vs-container loopback
  preflights;
- Docker troubleshooting covers `target_connect_failed`, `no_carrier_session`,
  benign non-IPv4 TUN noise, missing SOCKS listener and lease-without-carrier
  states.

The remaining low-priority item is optional healthchecks for deterministic
debug-carrier compose examples.
