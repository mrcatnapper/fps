# FPS Self Review

Date: 2026-05-29

## Findings

0. **C++ boilerplate hotspots are shrinking.** Enum stringification/numeric
   validation moved to Boost.Describe helpers, CLI tokenization moved to
   Boost.Program_options, wire-endian helpers moved to `fps/core/wire.hpp`,
   structured operational log tails use Describe-to-JSON, config parsing is
   split from shaper parsing, bridge IO is split from bridge lifecycle, and test
   CLI boilerplate is now centralized. The largest remaining hotspots are
   command-heavy relay CLI tests and relay CLI command execution, not protocol
   core.

1. **The project is beta-candidate, not production-ready.** Zero-RTT v5
   uses bidirectional carrier transcript binding plus an encrypted server-accept
   leg before final classified-record keys exist. Docker runtime, leased TUN
   routing, source-IP enforcement and quality checks are solid enough for
   controlled Linux/Docker trials. Public beta still needs independent protocol
   review, release-candidate soak policy and operator onboarding feedback.

2. **Lease enforcement is a security contract.** Server-side carrier metadata
   includes the assigned client IPv4. Inbound client TUN packets are dropped when
   IPv4 source does not match that lease, and server-to-client packets are routed
   by destination lease owner. Unit tests plus Docker multi-client simulation
   guard this behavior.

3. **Multi-client support is usable but still young.** Multiple UUID-backed
   clients can authenticate, receive distinct leases and share one server. The
   Docker multi-client smoke covers owner routing, spoof drop and liveness; the
   two-host soak additionally covered sustained mixed traffic, server-to-client
   lease routing, spoof drop and carrier restart/recovery for two clients.
   Inbound TUN fragment reassembly is now keyed by source carrier and packet id
   rather than one global slot, which removes a correctness risk for interleaved
   fragmented packets.

4. **Fuzzing now protects the right parser edges.** TLS record framing, covert
   frame decode, FPS envelope decode, Zero-RTT v5 client-auth/server-accept
   verification and TUN/control parsing have libFuzzer smoke targets. This does
   not replace protocol review or OpenSSL fuzzing.

5. **Documentation needed structure more than more pages.** Active public docs
   now live in `docs/`, developer/agent history lives in `dev/`, duplicate
   crypto/research artifacts were removed, and the root README is only a short
   entry point.

6. **Pre-production adapters are gone.** The active parser and Docker tooling no
   longer carry migration branches for removed config fields or old log counter
   layouts. That matches the current product stage, but public releases will
   need an explicit versioning policy before compatibility promises begin.

7. **Close diagnostics are now visible without a debugger.** Bridge sessions
   report non-secret close metadata, relay logs include `event=session_closed
   reason=...`, and status JSON carries `sessions.last_closed` plus a bounded
   `recent_closed` list. UUID generation is now raw-only through
   `fps_client --generate-client-uuid`, which keeps scripts and docs simpler.

8. **Auth and envelope failures now have operator-visible counters.** Status
   JSON no longer keeps auth under `sessions`; it exposes `auth` and `envelope`
   groups for candidates, authenticated sessions, precheck/unknown/decrypt
   misses and envelope encode/decode/tamper counters. A new local adversarial
   integration test exercises no-upgrade passthrough, unknown clients,
   transcript-prefix mismatch and post-auth tampering against live daemons.

9. **Replay state is now transcript-bound rather than cache-bound.** Active v5
   Zero-RTT has no timestamp or replay-cache fields. Replaying a client-auth
   candidate requires reproducing the same bidirectional carrier transcript and
   completing the server-accept leg before final FPS keys exist; durable replay
   state remains a possible protocol-review outcome if this assumption is
   judged too weak.

10. **The public-key-shaped Zero-RTT prefix is removed.** Candidates now start
    with transcript-bound opaque hints and keep the ephemeral public key inside
    the encrypted capsule. A future handshake-less classifier still needs
    separate review because false-positive/false-negative behavior can break the
    real browser/origin TLS stream.

11. **Remaining beta gates are mostly process gates.** The public GitHub remote,
    CI matrix and `main` branch protection are configured. The main blockers are
    external protocol review, release/signing workflow, repeated
    release-candidate soak and operator onboarding feedback.

12. **Manual real-origin carrier UX is documented, but not automated.**
    Ordinary long-lived browser/application TLS sessions can hold carriers and
    carry SOCKS/TUN traffic, but users need explicit guidance: an assigned lease
    is not enough, `carriers_current` must be positive, and public echo origins
    are not production dependencies.

13. **Router-hosted client looks plausible but remains unvalidated.** Running
    `fps_client` on a home router and using router DNS to point carrier
    hostnames at the router LAN address matches the current architecture. It
    still needs real OpenWrt/router validation for container runtime, CPU
    architecture, TUN support, capabilities and port binding before it can be
    marketed as supported.

14. **Carrier/datagram core is now less TCP-shaped.** `CovertDatagramTransport`
    accepts abstract carrier ids plus enqueue callbacks, and its direct unit
    tests use fake carriers without TCP sockets. The current concrete carrier is
    now explicitly named `TlsTcpCarrierSession`: it owns TLS record slicing,
    transcript tracking and classified-record insertion for TLS-over-TCP cover
    sessions. `make_tls_tcp_carrier_adapter(...)` is the thin boundary to the
    generic carrier pool, while `TunTunnelAdapter` keeps lease/source
    enforcement keyed by carrier metadata.

## Decisions Captured

- `security.zero_rtt` remains the only carrier authentication mechanism.
- Client identity remains UUID-only in user-facing config; server keys remain
  inline base64.
- Docker is the primary Linux deployment/runtime story.
- The base Docker image should stay FPS-only; application proxies belong in
  derivative overlay examples such as Dante, not in the core runtime contract.
- Product-level multi-client regression is Docker opt-in rather than ordinary
  local CTest.
- Client onboarding should move toward server-generated profiles and `fps://`
  URIs implemented in C++ CLI code, not broad helper scripts.
- Runtime health should stay local and metadata-only: `ops.status_socket` is
  read-only JSON with bounded recent close diagnostics and auth/envelope
  counters, not a management API.
- Structured log serialization should remain opt-in through described
  non-secret structs or non-secret views; secrets, UUIDs, keys and raw payloads
  must not become described log fields.
- Prefer Boost facilities when they reduce project-owned boilerplate without
  moving protocol semantics out of typed FPS code. Current good fits:
  Boost.Describe for enums/log labels, Boost.Program_options for argv parsing,
  Boost.JSON for config and Boost.Endian for wire integers.
- Do not add config/log migration branches until FPS has an external release
  that users could reasonably depend on.

## Recommended Next Actions

- Reuse the versioned `fps://` URI shape in future Android/GUI QR flows.
- Add CI jobs for GCC, clang, ASan/UBSan, Valgrind, coverage and fuzz smoke.
- Repeat the two-host Docker/TUN soak for release candidates and promote it to a
  privileged scheduled runner when the environment is stable enough.
- Decide after protocol review whether a replay cache should return on top of
  transcript-bound candidates.
- Keep `main` protected before inviting more contributors.
- Use the manual GHCR workflow with `publish=false` for release candidate image
  dry runs and `publish=true` for image publication.
- Keep `doctor` and deployment bundle tooling deferred until another manual UX
  pass confirms which checks and file layout operators actually need.
- Treat router/LAN-gateway mode as a documented experimental pattern until it
  passes a real hardware or close VM validation run.
- Continue the target split only where it reduces coupling: protocol,
  carrier/datagram, TUN adapter and Linux runtime now have separate CMake
  targets, but relay orchestration still composes the production TUN service
  directly because TUN is the only product adapter.
- Continue splitting relay runtime helpers by responsibility, starting with
  status service, TUN service, carrier registration/lease logic and
  CLI/profile/status/lease command helpers if `tcp_relay_app.cpp` grows again.
- Keep `TlsTcpCarrierSession` refactors focused on explicit state-machine helpers:
  socket IO, Zero-RTT transition, classified-record processing, shaper queues
  and half-close handling should not grow more tightly coupled.
- Plan UUID/key rotation and revocation workflow beyond basic lease
  prune/revoke.
