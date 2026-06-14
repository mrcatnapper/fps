# Android Client Plan

This developer note records the current Android client direction. It is a
handoff artifact, not operator documentation.

Testing methodology and emulator rollout are tracked in
[`ANDROID_TESTING_PLAN.md`](./ANDROID_TESTING_PLAN.md). Keep that file current
when Android test infrastructure or emulator strategy changes.

## Current Baseline

- Use Kotlin for the Android application layer and C++20/NDK for FPS native
  protocol code.
- Maintain a minimal Android app module that can be built from the command line
  without Android Studio.
- Keep ordinary Android build/unit checks reproducible in `Dockerfile.android`
  so host SDK/NDK/vcpkg paths are optional for CI and routine rebuilds.
- Build a JNI native library for `arm64-v8a` and `x86_64` that reuses:
  - the FPS IPv4 TCP/UDP 5-tuple parser for split-tunnel UID policy;
  - reusable protocol/datagram/TLS-TCP carrier sources that depend on OpenSSL
    and Boost.Asio.
- Keep the application headless while product behavior is still incomplete.
  The next Android milestone is not more isolated smoke coverage; it is a
  production-like runtime path that composes the existing pieces into one VPN
  lifecycle. The current service-owned runner drives native carrier auth,
  encrypted lease delivery, TUN establishment, split-tunnel policy and
  bidirectional datagram/TUN movement for a raw HTTPS cover profile and exposes
  a minimal foreground/status surface. HTTPS cover hardening, static
  shaper-profile UX and the first managed-device product-flow validation are
  implemented; the next product gaps are profile persistence/manual UX,
  real-device validation and later external-carrier design, not another
  internal carrier protocol.

## Implemented Headless Core Slice

Delivered:

- Android parses the current `fps://v1/<base64url-json>` client profile format
  and raw client JSON into a Kotlin `AndroidClientProfile`.
- Parsing accepts only client-side fields needed by the Android runtime:
  `network.server`, `security.zero_rtt.client_uuid`,
  `security.zero_rtt.server_public_key_base64`, optional codec, TUN, ops,
  carrier probe, split-tunnel and inline shaper metadata.
- Parsing rejects server-only secret/runtime fields such as
  `server_private_key_base64`, `allowed_client_uuids`, `tun.lease_pool`,
  `tun.server_address`, `tun.lease_file` and file-based `shaper.profile_file`.
- Parsing uses Android's `org.json` API. Headless JVM tests provide the same API
  through a test-only `org.json:json` dependency; do not maintain a project-local
  JSON parser for this boundary.
- Kotlin runtime state is modeled as a headless controller with platform hooks
  for VPN permission, TUN establishment, socket protection, underlying-network
  DNS and UID lookup.
- Carrier probe settings are modeled in Kotlin. The headless controller exposes
  carrier probe runtime plans and can own a deterministic carrier probe manager
  that resolves carrier endpoints through the underlying-network hook, calls
  socket protection before connect, drives fake HTTPS/WSS probe transports,
  tracks reconnect/backoff counters and exposes non-secret status.
- A live OkHttp probe transport factory exists for app-owned carrier keepalive
  traffic. It supports HTTPS GET and WSS carrier probe profiles, uses the
  already-resolved underlying-network endpoint through a per-transport DNS
  adapter and protects each Java `Socket` before connect through a platform
  hook. It is not an FPS wire carrier because OkHttp terminates TLS and does not
  expose raw TLS/TCP bytes to `TlsTcpCarrierSession`.
- The first `VpnService` lifecycle and TUN fd ownership layer exists. Service
  start parses a profile and waits for a server lease; after a lease arrives,
  `VpnService.Builder` installs the leased address and leased-subnet route when
  `tun.enabled=true` and owns the resulting `ParcelFileDescriptor` until
  controller stop.
- The headless controller exposes a non-secret runtime snapshot with state,
  last error, TUN presence/MTU and carrier probe status metadata for later
  UI/status integration.
- `FpsNativeRuntime` is the first JNI handle wrapper for native core ownership.
  It validates Android profile text in Kotlin, creates an opaque native runtime
  handle, exposes non-secret native snapshots and duplicates a TUN fd into
  native-owned RAII state without taking ownership from the Kotlin
  `ParcelFileDescriptor` holder. It also owns the first explicit native
  Boost.Asio `io_context` executor lifecycle with idempotent start/stop and
  deterministic no-op posted-command smoke coverage. The native binding is split
  into thin JNI entrypoints, runtime registry/state and object-conversion
  helpers.
- `HeadlessNativeVpnRuntime` is the current Kotlin lifecycle bridge between the
  headless VPN controller and the JNI runtime handle. It creates the native
  runtime, starts its executor after VPN permission is available, waits for the
  lease, establishes the Android TUN fd and attaches that fd to native as an
  owned duplicate. It then starts the native TUN pump skeleton. The managed
  emulator smoke verifies this path with a real `VpnService` fd and exercises
  explicit stop plus a debug revoke path. The pump reads from the duplicated fd,
  parses IPv4 TCP/UDP 5-tuples with shared native code and queues bounded
  metadata for Kotlin split-tunnel policy decisions. Packet bytes remain in
  native-owned state. Kotlin can complete packets as allow/drop and update
  non-secret counters. In this bridge, `allow` now attempts the native outbound
  packet seam. With no authenticated carrier transport installed yet, the
  default path returns `no_carrier_transport` and increments enqueue rejected
  counters rather than silently consuming the packet. Debug-only instrumented
  hooks can register an in-process fake carrier, proving that policy-allowed
  packets travel through the production `CovertDatagramTransport` path and
  produce metadata-only frame digests. The same datagram transport now delivers
  inbound server-to-client datagrams back to the native-owned duplicated TUN fd,
  exposing only non-secret write/drop counters.
- The JNI runtime can configure client auth metadata from the validated Android
  profile, rederive the UUID-backed client X25519 keypair in C++, validate the
  server public key encoding and run an in-memory native Zero-RTT auth smoke.
  That smoke uses the shared C++ auth/record/control codecs and emits a
  metadata-only encrypted TUN lease event for Kotlin. The native runtime can
  also bridge a protected raw carrier socket to a loopback local-cover socket
  through `TlsTcpCarrierSession` with real client-side Zero-RTT options. Managed
  emulator coverage verifies successful encrypted lease delivery and tampered
  server-accept failure on this bridge.
- Android profiles can now include the same compact inline static shaper CDF
  object as Linux configs. Kotlin parses and validates the JSON shape with
  Android's `org.json`, passes primitive CDF arrays through JNI, and native
  constructs the shared platform-neutral `fps::Shaper`. Android deliberately
  does not support `shaper.profile_file`; mobile profile import stays
  self-contained.
- The Docker-managed emulator lane now includes a debug-only product-flow smoke
  around the production coordinator shape. It starts a real Android
  `VpnService`, protects a raw carrier socket, runs the native TLS/TCP bridge,
  completes client-side Zero-RTT against a local test FPS server peer, receives
  an encrypted TUN lease, establishes a real TUN fd, attaches it to native and
  starts the native pump. The local cover side uses synthetic TLS Application
  Data records so the test remains deterministic and does not require an
  external origin.
- `HeadlessNativeVpnRuntime` now has the first product-shaped coordinator
  surface. It starts the native executor, resolves the FPS server through the
  underlying-network hook, prepares/protects/connects the raw carrier socket,
  starts the native bridge, starts a local cover-client hook against that
  bridge, drains native lease/auth events, establishes/attaches TUN, starts the
  pump and applies pending split-tunnel policy decisions.
- `CoordinatedNativeVpnRunner` wraps that one-attempt coordinator in a
  service-owned retry loop. It owns a runtime instance, the raw local cover
  starter and a scheduler; transient resolve/connect/bridge/cover/auth
  failures close the runtime and enter bounded exponential backoff, while
  missing VPN permission and invalid carrier profile remain terminal. The
  production `FpsVpnService` now starts this runner with
  `RawHttpsLocalCoverClientStarter`.
- `RawHttpsLocalCoverClientStarter` is the first real local cover-client
  implementation. It uses the first configured Android carrier profile, opens a
  TLS HTTPS GET keep-alive loop through the native loopback bridge and preserves
  raw TLS bytes for `TlsTcpCarrierSession`. It is separate from OkHttp probes,
  which still terminate TLS and remain health/probe support machinery.
- Android release native builds exclude debug/instrumented JNI test hooks. The
  release APK smoke verifies that `FpsNativeTestHooks` and auth/fake-carrier
  test entrypoints are not exported by `libfps_android_native.so`. The native
  runtime registry now copies a per-runtime entry under the global lock, then
  serializes calls with the entry mutex after releasing the registry mutex.
- Split-tunnel allowlist metadata is parsed into Kotlin and exercised through
  a fail-closed policy decision API backed by the platform UID lookup hook.
- Required verification remains Docker/JVM-first. Connected Android runtime
  checks stay opt-in.

## Product-Focused Next Plan

The project has enough Android "brick" tests for the current boundary. New
Android work should now prefer product-shaped integration checks over isolated
proofs unless an isolated test is needed to make a failing contract precise.

Target headless product flow:

```text
profile
  -> native runtime
  -> protected raw TCP socket
  -> local cover-client connection into native bridge
  -> TlsTcpCarrierSession with real Zero-RTT options
  -> encrypted server accept / TUN lease event
  -> VpnService TUN fd establishment
  -> outbound TUN policy
  -> CovertDatagramTransport enqueue
  -> inbound datagram write back to TUN
```

Tactical implementation order:

Completed product step:

- **Foreground service and status UX.**
  The smallest user-visible daemon surface exists: foreground notification,
  start/stop/status runtime seam and non-secret error reporting.
- **HTTPS cover hardening.**
  The raw HTTPS local cover client now bounds response draining through
  `max_response_bytes`, handles ordinary `Content-Length`/chunked/empty
  responses, reports metadata-only failures and stops its keep-alive worker
  cleanly on close.
- **Managed-device product-flow smoke.**
  The Docker-managed emulator lane validates the service-owned coordinator
  through protected raw socket connect, native bridge auth, encrypted lease,
  real `VpnService` fd attach and native pump startup.

Remaining tactical implementation order:

1. **Profile persistence and manual UX.**
   Add the smallest practical profile storage/import surface and manual test
   flow so the current headless service path can be exercised like a user-facing
   client.

2. **Real-device release validation.**
   Run the same HTTPS-only product path on at least one physical device to
   catch vendor VPN, background-service, network-switching and battery-policy
   differences that managed devices cannot model.

3. **External carrier design.**
   After the HTTPS path is stable, design external carrier support explicitly
   instead of adding raw WSS as a second internal carrier. That design must
   revisit DNS mapping, VPN-loop prevention and Android per-app routing.

Testing expectation:

- JVM tests validate coordinator sequencing, retry/backoff, service-runner
  lifecycle and fail-closed branches with fake platform/native backends.
- Managed-device tests validate Android framework behavior: protected fd, real
  `VpnService` fd and native bridge/auth/TUN wiring. Keep them deterministic and
  local; use real-device/manual checks for vendor-specific behavior.
- Do not add more "2 + 2" tests around already-stable helpers unless they
  protect a newly integrated product contract.

## Environment

- JDK: OpenJDK 21 is acceptable; AGP 9 requires JDK 17 or newer.
- Android SDK root: `/opt/android-sdk`.
- Required SDK packages:
  - `platform-tools`
  - `platforms;android-36`
  - `build-tools;36.0.0`
  - `ndk;28.2.13676358`
  - `cmake;3.22.1`
- Gradle is run through the repository wrapper, not the old system Gradle.

Useful shell setup:

```bash
export ANDROID_HOME=/opt/android-sdk
export ANDROID_SDK_ROOT=/opt/android-sdk
export ANDROID_NDK_HOME=/opt/android-sdk/ndk/28.2.13676358
export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"
```

Android OpenSSL is provided through the existing `/opt/vcpkg` tree only for the
Android build:

```bash
ANDROID_NDK_HOME=/opt/android-sdk/ndk/28.2.13676358 \
  /opt/vcpkg/vcpkg install openssl:arm64-android openssl:x64-android
```

Do not use vcpkg for Linux, Docker, Alpine or Boost in this project. Those
paths continue to use distro packages or the existing isolated Boost header
root.

The same environment is available without relying on host SDK paths:

```bash
tools/run_android_checks.sh
tools/run_android_checks.sh --docker
```

`Dockerfile.android` is a CI/build image, not an Android emulator image and not
the Linux product runtime image. Its source-free `android-gradle-base` stage
installs the Android SDK/NDK, Android OpenSSL triplets and Gradle dependency
cache. Outside Docker, `tools/run_android_checks.sh` defaults to building that
base image and running host Android checks with the current workspace
bind-mounted; host SDK use must be requested explicitly with `--host`. The final
source-containing `ci` stage is kept only for explicit image-shape experiments.

Post-JVM runtime checks use `Dockerfile.android-emulator`, a heavier child image
that adds the Android emulator and the API 30 AOSP ATD x86_64 system image. Run
it explicitly with `tools/run_android_checks.sh --docker-managed-device` on
hosts where `/dev/kvm` can be passed through to Docker. Repeated local runs
reuse the default `fps:android-ci-base` and `fps:android-emulator-ci` tags by
default and build only missing images. Set
`FPS_ANDROID_FORCE_DOCKER_REBUILD=1` only after Dockerfile layer, apt/sdk
package or base-image changes. The emulator image inherits from the source-free
`android-gradle-base` image and receives the current source tree through the
test container bind mount, so ordinary source edits do not force the
emulator/system-image layers to rebuild. Use
`tools/run_android_checks.sh --clean-images` for allowlisted Android image
cleanup instead of broad Docker prunes or pre-build tag deletion; it preserves
cache tags unless `FPS_ANDROID_CLEAN_TAGS=1` is set.

## Accepted Runtime Direction

- The first Android beta uses app-owned carrier sessions. The app opens and
  maintains HTTPS carrier traffic itself.
- Carrier/cover requests are configured at the Android layer: for example a
  periodic HTTPS GET. The existing OkHttp HTTPS/WSS code is support machinery
  for app-owned cover probes only. It must not become a direct FPS wire carrier
  because OkHttp terminates TLS and does not expose raw TLS record bytes.
  Production cover traffic should feed the native loopback bridge so
  `TlsTcpCarrierSession` remains the single FPS wire implementation. WSS may
  remain as probe metadata and future external-carrier research, but it is not
  an internal Android product carrier for the first versions.
- Real FPS carrier traffic must use native raw TCP/TLS stream handling through
  `TlsTcpCarrierSession`. Do not extend the OkHttp probe path into a second FPS
  wire protocol. The native runtime already exposes the first protected raw TCP
  lifecycle: prepare socket, return fd for `VpnService.protect(fd)`, complete
  connect or abort. The remaining work is to layer TLS/FPS auth and carrier
  registration on that socket.
- Carrier sockets must be protected before `connect`. Native/future sockets use
  the fd hook; OkHttp-owned sockets use the Java `Socket` hook.
- Hostname resolution for FPS/carrier endpoints must use Android's underlying
  `ConnectivityManager.Network`; native resolver behavior after VPN activation
  is not trusted.
- Startup is two-phase: authenticate and receive the server lease first, then
  create/configure the `VpnService` fd only for an Android profile with
  `tun.enabled=true`. Current native runtime wiring duplicates the fd through
  `HeadlessNativeVpnRuntime` and reports `tunFdOwnership=owned_duplicate`;
  the runtime executor lifecycle is explicit, the first native TUN read/parse
  pump starts after fd attachment, exposes a bounded policy metadata queue and
  emulator coverage exercises real fd attach/pump/stop/revoke. The first raw
  carrier socket lifecycle is covered through loopback TCP; native auth-core
  linkage is covered in memory; real protected-socket carrier registration
  remains the next native/JNI step.
- Split tunnel is the default. Full tunnel is an explicit advanced mode.
- Policy enforcement is fail-closed: parse TCP/UDP 5-tuples, resolve the owning
  UID with Android platform APIs, allow configured UIDs only, and drop malformed
  packets, unsupported protocols, unknown fragments and invalid UIDs.
- JNI/Kotlin entry points must post native operations onto the runtime
  `io_context`. Direct cross-thread carrier enqueue remains forbidden.

## Native Dependency Boundary

The Android native smoke links an explicit source list instead of the broad
Linux-oriented CMake targets:

- Header-only Boost.Describe, Boost.MP11 and Boost.Endian are usable in Android
  native code when exposed through an isolated Boost header root. Do not add
  `/usr/include` to Android targets.
- Boost.Asio/Boost.System are currently used header-only for the Android smoke
  through `BOOST_ERROR_CODE_HEADER_ONLY` and `BOOST_SYSTEM_NO_DEPRECATED`.
- OpenSSL is cross-built reproducibly for Android through vcpkg triplets and
  linked as `libcrypto.a`.
- Boost.Log stays behind `FPS_LOG_*`; Android uses a small `__android_log_print`
  stream backend for the logging macro instead of linking Boost.Log.
- Linux relay/config/profile operator code, Boost.JSON-heavy paths,
  Boost.Filesystem, Linux TUN device code and daemon CLI remain outside the
  Android native target.
- Profile parsing stays in Kotlin for app-level UX fields. Protocol-critical
  profile semantics must not be reimplemented in Kotlin; move them through JNI
  only after the JSON/base64/UUID helper boundary can build without dragging
  Linux daemon dependencies into the app.

## Verification

```bash
tools/run_android_checks.sh
tools/run_android_checks.sh --docker
tools/run_android_checks.sh --host
tools/run_android_checks.sh --connected
tools/run_android_checks.sh --managed-device
tools/run_android_checks.sh --docker-managed-device
```

`--connected` requires `adb devices` to show a device or emulator in the
`device` state. It installs and runs the instrumented native smoke on that
runtime; it is not part of ordinary CI.

`--managed-device` runs the Gradle Managed Device task with the current SDK and
emulator environment. `--docker-managed-device` builds the emulator child image
and runs the same task inside Docker with `/dev/kvm`. Keep these lanes opt-in
until repeated runs prove them stable enough for scheduled CI. GitHub Actions
has a manual-only `Android Emulator` workflow for this lane; it is not a
required PR check. The lane currently runs both the native/JNI smoke and a
debug-only real `VpnService.prepare(...)` / `VpnService.Builder.establish()`
smoke that requests VPN consent when needed, verifies a real TUN fd/MTU, routes
the fd through `HeadlessNativeVpnRuntime`, starts the native pump and closes it
through explicit stop/revoke paths.
