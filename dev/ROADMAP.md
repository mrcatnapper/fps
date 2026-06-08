# FPS Productization Roadmap

The near-term goal is to turn the verified transcript-bound Zero-RTT beta
candidate into a deployable Linux-first client/server VPN framework. Docker is
the primary runtime path.

Advanced full-flow shaping remains future work. The current priority is a
reliable tunnel, clear operator workflow and TLS-record-shaped wire behavior
without claiming resistance to advanced timing/size traffic analysis.

## Phase 1: Linux Runtime Basics

- [x] Narrow production identity UX to client UUID, server inline base64 keypair
  and `allowed_client_uuids`.
- [x] Remove pre-production key-file/client-key/public-key allowlist modes from
  user-facing config.
- [x] Remove pre-production config/log migration adapters before public release
  promises exist.
- [x] Split the covert datagram carrier core from the Linux TUN adapter so
  Android or future non-TUN adapters can reuse authenticated carriers and
  opaque datagram framing without inheriting IPv4 lease enforcement.
- [x] Split build targets into protocol core, datagram core, TLS/TCP carrier, TUN
  adapter and Linux runtime so future platform work can link narrower
  components without relying on a broad convenience aggregate.
- [x] Replace Linux-shaped TUN command callbacks with semantic `TunRuntime`
  link/address operations so platform code owns OS-specific configuration.
- [x] Make the generic carrier enqueue contract executor-explicit and reject
  wrong-thread synchronous enqueue calls before session queues are touched.
- [x] Move `fps://v1` client profile URI import and JSON validation into
  platform-neutral core so Android does not duplicate the profile format.
- [x] Add an injectable TCP socket-protection hook so Android can protect
  carrier sockets before connect without forking the relay connect path.
- [x] Add an outbound TUN packet policy hook with IPv4 TCP/UDP 5-tuple parsing
  so Android can enforce split-tunnel UID allowlists before covert enqueue.
- [x] Harden config UX: mode-specific validation, `--check-config`, non-secret
  summaries and example client/server configs.
- [x] Add server-owned IPv4 lease allocator and client `tun.auto_configure` for
  basic address assignment without DHCP.
- [x] Document Linux route/DNS workflow for TUN addresses, policy routing,
  split/full tunnel modes and cleanup.
- [x] Document the beta carrier-hostname workflow: explicit `/etc/hosts` mapping
  to the local client listener instead of a magical DNS proxy.

## Phase 2: Docker Runtime

- [x] Add Docker runtime baseline: one multi-stage image, entrypoint, compose
  examples for server and host-network client, static artifact tests.
- [x] Add smaller Alpine production Dockerfile with the same FPS runtime tools.
- [x] Add reusable WSS debug carrier generator in the common Docker image and
  integration tests.
- [x] Keep proxy daemons out of the base FPS image and document overlay proxy
  patterns; official Docker example uses a derivative Dante SOCKS5 image.
- [x] Harden L3 lease allocator: source-IP enforcement, destination lease
  routing, lease revoke/prune and operator listing.

## Phase 3: Documentation And Beta UX

- [x] Move active user/operator docs into `docs/`.
- [x] Move developer/agent working artifacts into `dev/`.
- [x] Remove stale research and duplicate crypto-review artifacts.
- [x] Translate active user docs and root README to English.
- [x] Implement server-generated client JSON profiles.
- [x] Implement an importable `fps://` client URI format.
- [x] Add safe profile `--output` and URI write-to-file import helpers.
- [x] Add a public beta quickstart that ties together Docker server/client,
  self-hosted carrier, `/etc/hosts`, status checks and proxy overlay.
- [x] Document real-origin carrier operation without FPS-specific carrier
  tooling, including persistent WebSocket carriers and SOCKS readiness signals.
- [x] Document router/LAN-gateway carrier DNS override as an experimental
  deployment pattern.

## Phase 4: Reliability And Operations

- [x] Add structured health/status surface: carriers, bytes, TUN drops,
  shaper/backpressure counters and session lifecycle.
- Add richer health/status surface: reconnect history and queue saturation
  history. Classified-record failures are now exposed as status counters.
- Add operator `doctor` tooling and deployment bundle generation after more
  user-flow feedback; keep the current increment documentation-only.
- Add graceful reload for allowlist/config parts where it is safe without
  recreating active sessions.
- Implement reconnect/backoff and resilient client daemon mode for carrier,
  origin or server loss.
- [x] Enforce duplicate-UUID handling with the single supported policy:
  `replace_old`, where a new active device/profile supersedes older carriers for
  the same UUID.
- [x] Add a short Docker/TUN resilience soak smoke for carrier loss/recovery,
  spoof-drop liveness, mixed UDP/TCP traffic and status-counter assertions.
- [x] Keep Docker strict `write_queue_full` as diagnostic until saturation is
  deterministic; cover lease-aware queue overflow in unit tests.
- [x] Harden inbound TUN fragment reassembly for multi-carrier use by keying
  reassembly state by source carrier and packet id with a bounded state cap.
- [x] Run a two-host Docker/TUN soak on a weak remote Linux host for the current
  beta candidate. The current repository gate is a reproducible 300-second
  split-host run; longer runs remain release-candidate policy, not a single
  historical guarantee.
- [x] Add a repository two-host soak tool so release-candidate split-host
  checks do not depend on ad-hoc scripts.
- Repeat `tools/docker_two_host_soak.py` for release candidates until it is
  promoted to a privileged scheduled runner.

## Phase 5: Security Hardening

- Regenerate a concise external protocol review brief from the current
  specification, beta status, tests and latest soak evidence.
- Run independent crypto/protocol review of transcript-bound precheck,
  classified FPS records, shaper interaction and replay assumptions.
- [x] Design and implement the next Zero-RTT wire revision around a full per-direction carrier
  transcript hash, removing visible public-key-shaped handshake material and
  reducing replay surface.
- [x] Replace post-auth carrier wrapping with classified FPS records: ordinary
  carrier TLS records are forwarded byte-for-byte, while opaque
  datagram/control data is inserted as separate hint-classified TLS Application
  Data records.
- [x] Add first size-aware shaper increment: inserted classified FPS records can
  be padded to planner-selected full TLS record wire sizes with commit-safe
  queue/budget handling.
- [x] Add first adaptive shaper baseline: shared per-process TLS-record
  size/delay CDF training, delayed client upgrade warmup and encrypted server
  snapshot bootstrap for clients.
- Add pcap-level statistical shaper checks for packet-size and timing
  distributions over representative carrier profiles.
- Decide from pcap experiments whether independent adaptive CDFs are sufficient
  or whether size/time correlation needs a copula or another joint model.
- After review, decide whether transcript-bound one-time hints can evolve into
  a handshake-less FPS record classifier inspired by stream-transcript covert
  channels, or whether explicit upgrade plus classified-record insertion remains
  the safer product baseline.
- [x] Document UUID/client revocation and server-key rotation workflows beyond
  the lease tools.
- [x] Exercise UUID/client revocation and server-key rotation on a Docker
  candidate deployment.
- Repeat the rotation drill for release candidates or after changing
  profile/lease tooling.
- Keep UUIDs as per-device/per-profile bearer secrets; do not add group/shared
  UUID semantics for L3 VPN mode.
- [x] Remove active timestamp/replay-cache fields from Zero-RTT v5 and document
  bidirectional transcript plus server-accept replay assumptions.
- [x] Add first adversarial local tests for no-upgrade passthrough,
  unknown-client, transcript-prefix mismatch and post-auth carrier tamper.
- [x] Expand negative integration tests into unknown-client storms,
  transcript-mismatch bursts, recovery after failed auth and post-auth tamper
  checks without logging secrets.
- Keep active CPU DoS review as a protocol-review item; current tests cover
  bounded storm regressions, not a formal CPU budget guarantee.

## Phase 6: Platform And CI

- [x] Add GitHub Actions baseline for `ubuntu-24.04 x gcc/clang`, Docker
  build/smoke for both compilers and scheduled/manual quality checks.
- [x] Configure the GitHub remote and pass the initial CI matrix.
- [x] Add GitHub operations notes for artifact/secret scans, branch protection,
  image publication planning and protocol review packet.
- [x] Select MIT license and add a beta release checklist.
- [x] Add a manual GHCR image workflow with `publish=false` dry runs and
  `publish=true` publication after tag policy and permissions review.
- Add release signing/publishing, release upgrade docs and an opt-in privileged
  root/TUN CI job before public beta.
- Check Linux distro compatibility: Ubuntu LTS first, then Debian stable.
- Document minimal kernel capabilities and deployment topology.
- Validate OpenWrt/router-hosted `fps_client` on real hardware or a close lab
  target before calling it supported.
- [x] Add a command-line Android/Kotlin/NDK bootstrap with a minimal
  `VpnService` shell, headless policy tests and native `arm64-v8a`/`x86_64`
  build smoke for reusable C++ TUN 5-tuple parsing.
- [x] Extend Android native smoke to reusable protocol/datagram/TLS-TCP carrier
  sources with Android OpenSSL and Boost.Asio, without importing Linux
  daemon/config/runtime code.
- [x] Add a reproducible Android Docker build/test image plus an opt-in
  connected instrumented native smoke for device/emulator runtime validation.
- [x] Add a headless Android profile/runtime layer for carrier probe metadata,
  split-tunnel UID allowlists, carrier endpoint resolution and two-phase
  lease-before-TUN state transitions with fake platform hooks.
- [x] Add a deterministic headless carrier probe runner with fake transports,
  protect-before-connect ordering, reconnect/backoff counters and JVM tests.
- [x] Add OkHttp-backed live HTTPS/WSS Android carrier probe transports with
  Java-socket protection, resolved-endpoint DNS and JVM MockWebServer tests.
- [x] Add first Android `VpnService` lifecycle and TUN fd ownership layer:
  profile start/stop, lease-triggered `VpnService.Builder` establishment and
  idempotent fd close.
- [x] Add first JNI native runtime handle/snapshot boundary with native-owned
  duplicate TUN fd attachment.
- [x] Add first JNI native runtime executor lifecycle with idempotent
  start/stop and posted-command smoke coverage.
- Continue Android implementation from `dev/ANDROID_BOUNDARY.md` and
  `dev/ANDROID_APP_PLAN.md`: implement native raw TLS/TCP auth/TUN pump wiring,
  Android lifecycle resilience/status and richer native/Kotlin async commands.

## Phase 7: Fuzzing And Soak

- [x] Add first bounded libFuzzer smoke for FPS-owned parsers/codecs.
- Expand fuzz corpora and add nightly long-running fuzz/minimization jobs.
- [x] Add Docker multi-client soak smoke with reconnect, backpressure and
  sustained traffic.
- [x] Validate one distributed two-host Docker/TUN soak over a real published
  `:443` carrier port.
- [x] Add reproducible two-host soak orchestration with local image transfer,
  multi-client/multi-carrier traffic, carrier restarts and bad-log gates.
- Promote Docker/TUN soak to scheduled/manual privileged CI after repeated
  stable release-candidate runs.

## Deferred: Advanced Shaping

- Full-flow visible TLS record size/timing shaping.
- Classifier regression lab for representative carrier profiles.
- Statistical assertions for long-running traffic distributions.
