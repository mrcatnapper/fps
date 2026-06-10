# Android Architecture Review, 2026-06-10

This is an ad hoc source-level review of the current Android client slice. The
first pass intentionally ignored the existing Android planning documents and
looked only at code, tests and shared core boundaries. The goal is to identify
whether the Android implementation is drifting away from the intended product:

> an Android VPN client that reuses the already-tested FPS core as much as
> possible, and differs from an OpenVPN-like Android client mainly by owning and
> maintaining carrier TLS sessions.

## Current Source Baseline

- Kotlin owns Android platform orchestration: `VpnService`, permission/TUN
  hooks, UID-based split-tunnel policy, profile parsing and headless runtime
  state.
- JNI owns the native runtime handle, native executor lifecycle, native TUN fd
  duplicate, protected raw TCP socket lifecycle, test hooks and snapshots.
- Native C++ already links reusable FPS pieces: crypto, Zero-RTT controller,
  TLS record parsing, classified record codec, `CovertDatagramTransport`,
  `TlsTcpCarrierSession` and TUN packet parsing.
- Android has good boundary smoke coverage: JVM tests for profile/controller
  behavior and managed-device tests for native library loading, TUN fd
  ownership, TUN packet parsing, policy queues, fake carrier enqueue and a raw
  TLS/TCP passthrough bridge.

The direction is broadly correct: Android is not reimplementing the FPS
protocol in Kotlin. The main problems are incomplete production composition and
several temporary smoke seams that are starting to look like real API.

## High-Risk Findings

### 1. `FpsVpnService` Is Not Yet A Real Daemon Loop

Evidence:

- `FpsVpnService.startProfile(...)` creates `HeadlessNativeVpnRuntime` and calls
  `start()`, then stops there.
- Production service code does not call:
  - raw carrier connect/protect/bridge startup;
  - carrier cover-client startup;
  - native event draining;
  - lease application loop;
  - pending TUN policy draining/completion loop.
- Those steps exist as manual methods on `HeadlessNativeVpnRuntime`, not as an
  owned service lifecycle.

Impact:

- The service can successfully start and enter `WAITING_FOR_LEASE`, but no
  production path currently drives it to authenticated carrier, lease, TUN and
  packet forwarding.
- Tests validate individual pieces, but not the product lifecycle that a user
  would actually run.

Recommended fix:

- Add a small explicit Android runtime coordinator owned by `FpsVpnService` or
  by `HeadlessNativeVpnRuntime`.
- It should sequence:
  1. start native runtime;
  2. resolve/connect/protect raw carrier socket;
  3. start native TLS/TCP carrier bridge;
  4. start the app-owned cover client against the bridge listener;
  5. drain native events until lease is received;
  6. establish TUN;
  7. start TUN pump;
  8. periodically drain and complete split-tunnel policy packets;
  9. reconnect carriers on close/backoff.

### 2. Raw Carrier Bridge Is Passthrough-Only

Evidence:

- `start_raw_carrier_bridge_on_worker()` builds `TlsTcpCarrierSession` with
  `passthrough_pipelines()` and an empty `TlsTcpCarrierSessionConfig{}`.
- No `TlsTcpCarrierZeroRttOptions` is installed on that session.
- Therefore no Android raw bridge path currently performs v5 Zero-RTT auth,
  server accept handling, classified-record activation, lease delivery or
  authenticated datagram transport.

Impact:

- The raw bridge proves that native can protect/connect a socket and pass
  TLS-record-shaped bytes through `TlsTcpCarrierSession`.
- It is not yet a production FPS carrier. If TUN packets are allowed through
  this bridge today, enqueue attempts cannot use the active v5 classified
  record path.

Recommended fix:

- Build production `TlsTcpCarrierZeroRttOptions` from the Android profile:
  - UUID-derived client X25519 keypair;
  - configured server public key;
  - profile id;
  - client upgrade delay and sigma;
  - codec/frame/padding limits;
  - random `ClientInstanceId` encoded as client-auth payload.
- Attach `on_zero_rtt_authenticated`, `on_covert_frame`,
  `on_zero_rtt_upgrade_error`, `on_closed` and classified-record diagnostics to
  the Android bridge.
- Decode server accept control payload through the existing TUN lease control
  codec and emit the existing metadata-only `lease_received` event.

### 3. Inbound Server-To-Client TUN Delivery Is Missing

Evidence:

- Android native runtime owns a `CovertDatagramTransport`, but constructs it
  without an `on_datagram` handler.
- Raw bridge `on_covert_frame` calls
  `datagram_transport_.handle_covert_frame(...)`.
- There is no corresponding path from inbound decoded datagram to `write()` on
  the duplicated Android TUN fd.

Impact:

- Current Android native TUN pump only covers outbound TUN reads and outbound
  enqueue attempts.
- Even after real classified records are decoded, server-to-client VPN traffic
  will be dropped unless inbound datagrams are connected to TUN writes.

Recommended fix:

- Add an inbound datagram handler to Android native runtime.
- It should write packets back to the native-owned duplicated TUN fd using the
  same executor/lifecycle discipline as reads.
- Track non-secret counters: packets/bytes written, write failures and queue
  drops.
- Prefer reusing or adapting the existing `TunPacketPump` write path instead of
  adding another unrelated write queue.

## Medium-Risk Findings

### 4. OkHttp Carrier Probe Path Can Become A False Product Path

Evidence:

- `OkHttpCarrierProbeTransport` opens HTTPS/WSS through OkHttp and protects Java
  sockets.
- It does not connect to the native raw bridge listener.
- OkHttp terminates TLS and cannot expose raw TLS record bytes to
  `TlsTcpCarrierSession`.

Impact:

- It is useful as a keepalive/probe harness, but not as the actual FPS carrier.
- If grown further, Android can accidentally end up with two unrelated carrier
  systems: one visible keepalive and one native FPS transport.

Recommended fix:

- Keep OkHttp only as optional app-owned cover-client machinery.
- For production FPS, the app-owned cover client must connect to the native
  loopback bridge port, not directly to the remote origin/server path.
- Document and test this distinction at the code level before adding more
  carrier modes.

### 5. Test Hooks Are Exposed In The Production Native Library

Evidence:

- Native declarations include `run_client_auth_smoke_for_test`,
  fake carrier hooks and capture sinks.
- JNI exports for those hooks are compiled into the same native shared library.

Impact:

- Kotlin test wrappers are debug-only, but native symbols and code still ship
  unless the build gates them.
- This increases attack surface and makes production/test responsibilities less
  clear.

Recommended fix:

- Put test hooks behind an explicit debug/test compile definition.
- Keep production JNI surface limited to runtime lifecycle, carrier lifecycle,
  TUN attach/pump, policy completion, status/snapshots and event drain.

### 6. Native Runtime Registry Holds A Global Mutex Across Blocking Runtime Calls

Evidence:

- Registry methods lock `mutex_`, find a runtime and then call methods such as
  `prepare_raw_carrier_socket`, `complete_raw_carrier_protection`,
  `start_raw_carrier_bridge`, `stop`, and smoke auth.
- Several runtime methods post to Asio and wait on futures.

Impact:

- Current call graph does not obviously deadlock, but this is a fragile Android
  lifecycle boundary.
- Future callbacks, close paths or service stop calls can become difficult to
  reason about.

Recommended fix:

- Store runtimes as `std::shared_ptr`.
- Under registry lock, only find/copy the shared pointer.
- Release the lock before calling runtime methods that can block, join threads
  or post work.

### 7. Android TUN Policy Queue Is A Parallel TUN Stack

Evidence:

- Android native runtime directly reads from fd, parses IPv4 flow tuples,
  queues pending/in-flight policy packets and enqueues allowed packets into
  `CovertDatagramTransport`.
- Core already has `TunTunnelAdapter` and `TunPacketPump`, but their current
  policy hook is synchronous.

Impact:

- The custom Android loop is understandable because Android UID policy is
  asynchronous across JNI/Kotlin.
- If it keeps growing, packet/drop/write behavior will diverge from Linux TUN
  semantics.

Recommended fix:

- Treat the Android queue as a candidate for a reusable
  `AsyncTunPolicyGate`/`PendingTunPolicyQueue` core component.
- Keep Android-specific logic limited to resolving UID and returning
  allow/drop.
- Reuse shared packet parsing, datagram enqueue and inbound TUN write behavior.

## Low-Risk / UX Findings

- `FpsVpnService` has no foreground-service notification, profile persistence,
  restart-after-process-death handling or user-facing permission flow yet. This
  is acceptable for headless development but must be closed before a usable APK.
- Native snapshots are large and constructor signatures are brittle. This is
  manageable for now, but a future typed JNI status object or JSON status
  snapshot may reduce maintenance cost.
- Android profile parsing in Kotlin is fine. It avoids dragging Linux daemon
  config and Boost.JSON into the app; keep it client-profile-only.

## What Is Not A Problem

- Kotlin is not duplicating crypto, Zero-RTT or classified-record logic. Native
  core still owns those.
- Kotlin split-tunnel policy is the right layer for Android because
  `ConnectivityManager.getConnectionOwnerUid(...)` is a platform API.
- Keeping profile parsing in Kotlin is acceptable because Android has a
  platform JSON implementation and the app only needs the client profile shape.
- The current managed-device tests are useful. The gap is product composition,
  not lack of basic boundary coverage.

## Recommended Tactical Order

1. Convert the raw bridge into a real Zero-RTT Android carrier.
   - Add production `TlsTcpCarrierZeroRttOptions`.
   - Emit real lease events from server accept payload.
   - Register authenticated carrier frames through the existing
     `CovertDatagramTransport` path.

2. Add inbound datagram-to-TUN writes.
   - Prefer reusing/adapting `TunPacketPump` semantics.
   - Add managed-device tests that send an inbound datagram frame and observe a
     write to the duplicated TUN/test fd.

3. Add a daemon coordinator loop.
   - Drive carrier connect/protect/bridge, cover-client attach, native event
     drain, TUN establishment and policy draining.
   - Keep it headless/testable before adding UI.

4. Gate test-only JNI/native hooks.
   - Production shared library should not export fake carrier and capture-sink
     helpers by default.

5. Reduce registry mutex scope.
   - Avoid holding global registry lock during blocking runtime operations.

6. Re-evaluate OkHttp carrier probe code.
   - Keep it only if it becomes the app-owned cover client that connects to the
     native bridge.
   - Otherwise, mark it as debug/probe-only and avoid building product logic on
     it.

## Acceptance Target For The Next Android Milestone

A headless managed-device test should be able to run this single production-like
flow:

1. Start `FpsVpnService` or `HeadlessNativeVpnRuntime` with a client profile.
2. Native opens and Kotlin protects the raw carrier socket.
3. Native starts a TLS/TCP bridge with real Zero-RTT options.
4. A local cover client connects to the bridge and exchanges TLS-record-shaped
   bytes with a local fake server peer.
5. Zero-RTT server accept payload produces a lease event.
6. Android establishes/attaches TUN.
7. A packet written to TUN is policy-checked and encoded into authenticated
   classified FPS records.
8. An inbound authenticated datagram is written back to TUN.

Until that flow exists, Android should be treated as a strong boundary/testing
scaffold, not as a working VPN client.
