# Beta Status

## Verdict

FPS is ready for controlled Linux/Docker beta deployments. The current tree is
coherent: transcript-bound Zero-RTT is the only authentication path, the
covert datagram core is separated from the Linux TUN VPN adapter, leased TUN
routing is strict, Docker is the primary runtime, proxy daemons are documented
as overlays, and the local/CI quality workflow is defined.

FPS is not ready for public production or an unattended public beta. The main
remaining work is independent protocol review, repeated release-candidate soak
coverage, operator onboarding feedback and release-signing policy. Manual GHCR
image publication exists for controlled beta artifacts; public GitHub Releases
and signed artifacts remain deferred.

## Ready Now

- Zero-RTT is the only carrier authentication mechanism in active config, CLI
  helpers and regression tests.
- Client identity is one per-device/per-profile secret UUID. Server identity is
  an inline base64 X25519 key pair plus `allowed_client_uuids`.
- Duplicate UUID handling enforces `replace_old`: a per-process encrypted client
  instance id lets the server keep multiple carriers from one client process
  while replacing older carriers when another active instance reuses the same
  UUID/lease.
- Config parsing, CLI helpers, generated profiles and Docker simulations use the
  current schema without pre-production compatibility adapters.
- Server-owned IPv4 leases are implemented. The server routes packets to the
  carrier that owns the destination lease and drops client packets whose IPv4
  source does not match the assigned lease.
- The carrier/data path is split into reusable `CovertDatagramTransport`,
  explicit TLS-over-TCP carrier sessions (`TlsTcpCarrierSession`) and the
  current `TunTunnelAdapter`. TUN is the first and most important product
  adapter for VPN service, but the datagram transport no longer assumes that
  every covert payload is an IP packet. `TunTunnelAdapter` now registers generic
  `CovertCarrier` handles by `CarrierId`, while the Linux runtime owns the
  concrete TLS/TCP session mapping.
- Docker is the primary Linux runtime path. The base image contains FPS binaries,
  `fps_carrier`, route/debug tooling and the operator entrypoint, but no embedded
  SOCKS/HTTP proxy daemon.
- The official proxy example is an overlay: a derivative Dante SOCKS5 image in
  `examples/docker/proxy-dante`. Squid is documented as an HTTP proxy pattern,
  not as a maintained compose example.
- Ubuntu and Alpine Dockerfiles are covered by Docker smoke checks.
- `fps_carrier origin/client` provides a self-hosted HTTPS/WSS echo carrier for
  deterministic operator smoke tests, browser carrier creation and integration
  tests.
- Browser-created carrier sessions use explicit hostname mapping such as
  `/etc/hosts`; FPS intentionally does not include a beta DNS proxy.
- Client onboarding has generated JSON profiles, safe `0600` output writing and
  a compact `fps://v1` URI transport.
- The public beta quickstart documents the Docker-first operator flow: server
  compose, self-hosted carrier, generated client profile, `/etc/hosts` carrier
  mapping, client host-network compose, status checks and optional Dante proxy
  overlay.
- Real-origin carrier documentation now covers browser/application carrier
  sessions without `fps_carrier`, including persistent WebSocket carriers and
  the distinction between an assigned lease and a live carrier.
- Router/LAN-gateway carrier DNS override is documented as an experimental
  deployment pattern, but OpenWrt-style hosts are not yet tested release
  targets.
- UUID/client revocation and server key rotation are documented as explicit
  restart/redeploy procedures and have been exercised in a local Docker
  rotation drill.
- The repository has an MIT license and a release checklist for beta candidates.
- Runtime status is available through an opt-in local UNIX socket and `--status`,
  exposing non-secret session, recent-close, auth, classified-record, carrier,
  TUN and shaper counters.
- Repetitive queue/backpressure logs are time-aggregated; status counters remain
  the source of exact totals.
- GitHub Actions workflow files exist for `ubuntu-24.04 x gcc/clang` local
  CTest and Docker smoke, plus scheduled/manual quality checks for sanitizers,
  Valgrind, coverage and bounded fuzzing.
- Android has a reproducible Docker build/test image and a headless Kotlin/NDK
  scaffold. Current JVM tests cover client profile parsing, carrier probe
  planning, split-tunnel UID allowlists, underlying-network endpoint resolution,
  socket-protection failure handling, deterministic fake-transport carrier
  probe runner lifecycle/reconnect behavior, live OkHttp HTTPS/WSS probe
  transport tests, native-runtime wrapper tests and two-phase lease-before-TUN
  state transitions without requiring an emulator.
- A manual GHCR publishing workflow exists for Ubuntu and Alpine runtime
  images. It defaults to dry-run mode and requires an explicit `publish=true`
  dispatch input before pushing images. It publishes only two tags per run: the
  unsuffixed Ubuntu image tag and the `-alpine` image tag.
- Regression coverage includes local unit/integration tests, clang-20,
  ASan/UBSan, Valgrind unit checks, llvm-cov, bounded libFuzzer smoke, opt-in
  TUN tests and opt-in Docker simulations.
- The multi-client Docker simulation proves that two UUID clients receive
  distinct leases, valid traffic is routed to the owner and a spoofed source is
  dropped while the stack remains alive. A separate duplicate-UUID Docker
  simulation covers `replace_old`.
- A short Docker/TUN resilience soak harness now exercises mixed UDP/TCP
  traffic, carrier loss/recovery, spoof-drop liveness, optional Dante overlay
  probing and status-counter assertions.
- The current release-candidate workflow includes a reproducible two-host
  Docker/TUN soak with the server stack on a weak remote Linux host and the
  clients on the local host. The repository tool exercises two UUID clients over
  the published server `:443` carrier port, sustained UDP plus HTTP probes,
  server-to-client lease routing, spoof-source drop validation and planned
  carrier restarts.
- Queue saturation has deterministic unit coverage for bounded write queues and
  lease-aware routing. Docker-level strict `write_queue_full` observation stays
  diagnostic because it depends on host timing and container throughput.
- Local adversarial Zero-RTT coverage now checks no-upgrade passthrough,
  unknown-client storms, transcript-prefix mismatch bursts, recovery after
  failed auth and post-auth carrier tamper against live FPS daemons with
  status-counter assertions and secret-leak checks.
- Timestamp and replay-cache fields are not active Zero-RTT v5 config or wire
  features. Replay resistance relies on the bidirectional carrier transcript
  prefix plus the server accept leg; durable replay state remains a possible
  protocol-review outcome.

## Repository And CI State

Current state:

- The repository is hosted publicly on GitHub.
- Branch protection is configured for `main`: pull requests, current CI status
  checks, linear history, resolved conversations and no force-pushes/deletions.
- Public release tags and signed artifacts are not expected yet.
- Image publication, when used, is manual GHCR publication with explicit
  pre-release tags, no `latest` and no implicit provenance/SBOM package
  artifacts.

Next release-readiness actions:

- Keep the ordinary local suite and Docker artifact check from
  [testing.md](./testing.md) as the pre-PR baseline.
- Run artifact and secret scans before publishing release candidates.
- Trigger the scheduled/manual `Quality` workflow at least once for release
  candidate branches or before sharing images with external beta users.
- Keep release image publication manual until tag aliases and signing are
  reviewed.

The current beta phase does not require signed images or privileged root/TUN CI.
Those remain release-hardening concerns.

## Beta Risks

- Public onboarding now has documented Docker-first, debug-carrier and
  real-origin carrier paths, but it still needs real beta feedback before being
  treated as polished consumer UX.
- Router-hosted `fps_client` deployments need validation on real hardware,
  especially around CPU architecture, container runtime, TUN permissions, port
  binding and router DNS behavior.
- UUIDs are per-device/profile secrets. Shared UUIDs are unsupported; continuous
  reconnect fights between two processes using one UUID remain an operator
  misconfiguration, even though `replace_old` prevents simultaneous L3 sharing.
- Long-running daemon resilience is now manually validated for the current
  candidate, but not automated. Repeat the two-host soak for release candidates
  until a privileged scheduled runner exists.
- A current external protocol review brief has not been regenerated after the
  latest transcript-bound/classified-record/shaper changes. There is still no
  independent cryptographic/protocol review of the active construction.
- UUID/client revocation and server-key rotation passed one local Docker drill,
  but this remains an operator procedure rather than a hot-reload management
  API.
- The transcript-bound precheck removes the visible public-key-shaped handshake
  prefix and has storm regression coverage, but it is not a full CPU DoS
  defense. Plausible server hints still force client-hint allowlist checks and
  a small AEAD attempt for a likely client.
- Operational status is still minimal: it is a local JSON snapshot with bounded
  recent close metadata and auth/classified-record counters, not a metrics
  endpoint, management API or historical time series.
- Release engineering remains incomplete for public beta: no signed Docker
  images, public GitHub Release process, public upgrade guide or privileged
  root/TUN CI runner.
- Traffic-shape mimicry remains incomplete. FPS now has classified-record
  padding, adaptive CDF training and shaper-aware fragmentation, but it does not
  claim timing/size distribution resistance.
- Android remains pre-application: the headless profile/runtime boundary,
  OkHttp app-owned HTTPS/WSS carrier probes, first `VpnService` TUN fd ownership
  layer, split JNI native runtime handle and headless Kotlin/native lifecycle
  bridge exist, but native raw TLS/TCP auth/pump wiring, Android lifecycle
  resilience/status and UI are not implemented yet.

## Public Beta Gate

Before public beta or public release, finish:

- **Release matrix hardening:** before public release, add signed Docker images,
  public release publishing, public upgrade guidance and a documented opt-in
  root/TUN CI job.
- **Soak repeat policy:** keep the two-host Docker/TUN soak as a required
  release-candidate check and automate it later on a privileged runner. Public
  release should not rely on a single historical pass.
- **Independent protocol review:** regenerate a concise current review brief
  from `docs/specification.md`, `docs/testing.md`, this status page and the
  relevant tests, send it to an external reviewer, then resolve findings around
  transcript binding, hint precheck, classified FPS records and the
  no-timestamp/no-cache replay model.
- **Operator onboarding feedback:** run the documented quickstart with beta
  operators and reduce friction found in real deployments.
- **Rotation repeat policy:** repeat the UUID revoke/reissue and server-key
  rotation drill for release candidates or after changing profile/lease tooling.

## Deferred After Beta

- Full Android application implementation.
- Authenticated/user-scoped proxy overlay examples.
- Full timing/size shaper and traffic-analysis lab.
- IPv6 lease allocation.
