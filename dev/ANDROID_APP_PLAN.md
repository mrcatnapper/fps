# Android Client Plan

This developer note records the current Android client direction. It is a
handoff artifact, not operator documentation.

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
- Keep the scaffold mostly headless: no real VPN lifecycle, GUI or native TUN
  pump yet. A connected instrumented smoke exists, but it is opt-in and runs
  only when an external Android device or emulator is attached.

## Implemented Headless Core Slice

Delivered:

- Android parses the current `fps://v1/<base64url-json>` client profile format
  and raw client JSON into a Kotlin `AndroidClientProfile`.
- Parsing accepts only client-side fields needed by the Android runtime:
  `network.server`, `security.zero_rtt.client_uuid`,
  `security.zero_rtt.server_public_key_base64`, optional codec, TUN, ops,
  carrier probe and split-tunnel metadata.
- Parsing rejects server-only secret/runtime fields such as
  `server_private_key_base64`, `allowed_client_uuids`, `tun.lease_pool`,
  `tun.server_address` and `tun.lease_file`.
- Parsing uses Android's `org.json` API. Headless JVM tests provide the same API
  through a test-only `org.json:json` dependency; do not maintain a project-local
  JSON parser for this boundary.
- Kotlin runtime state is modeled as a headless controller with platform hooks
  for VPN permission, TUN establishment, socket protection, underlying-network
  DNS and UID lookup.
- Carrier probe settings are modeled in Kotlin. The headless controller exposes
  carrier runtime plans and can own a deterministic carrier manager that
  resolves carrier endpoints through the underlying-network hook, calls socket
  protection before connect, drives fake HTTPS/WSS probe transports, tracks
  reconnect/backoff counters and exposes non-secret status.
- A live OkHttp transport factory exists for app-owned carrier sockets. It
  supports HTTPS GET and WSS carrier profiles, uses the already-resolved
  underlying-network endpoint through a per-transport DNS adapter and protects
  each Java `Socket` before connect through a platform hook.
- Split-tunnel allowlist metadata is parsed into Kotlin and exercised through
  a fail-closed policy decision API backed by the platform UID lookup hook.
- Required verification remains Docker/JVM-first. Connected Android runtime
  checks stay opt-in.

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
the Linux product runtime image. It installs the Android SDK/NDK and Android
OpenSSL triplets, then runs the host Android checks inside the container.
Outside Docker, `tools/run_android_checks.sh` defaults to this Docker path; host
SDK use must be requested explicitly with `--host`.

## Accepted Runtime Direction

- The first Android beta uses app-owned carrier sessions. The app opens and
  maintains HTTPS/WSS carrier traffic itself.
- Carrier requests are configured at the Android layer: for example a periodic
  HTTPS GET or a WSS stream probe. The current headless runner tests lifecycle
  with fake transports, and the OkHttp transport factory provides the first live
  HTTPS/WSS socket implementation without changing the FPS protocol.
- Carrier sockets must be protected before `connect`. Native/future sockets use
  the fd hook; OkHttp-owned sockets use the Java `Socket` hook.
- Hostname resolution for FPS/carrier endpoints must use Android's underlying
  `ConnectivityManager.Network`; native resolver behavior after VPN activation
  is not trusted.
- Startup is two-phase: authenticate and receive the server lease first, then
  create/configure the `VpnService` fd and start the native TUN pump.
- Split tunnel is the default. Full tunnel is an explicit advanced mode.
- Policy enforcement is fail-closed: parse TCP/UDP 5-tuples, resolve the owning
  UID with Android platform APIs, allow configured UIDs only, and drop malformed
  packets, unsupported protocols, unknown fragments and invalid UIDs.
- JNI/Kotlin entry points must post native operations onto the FPS `io_context`.
  Direct cross-thread carrier enqueue remains forbidden.

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
- Profile parsing should be reused from C++ only after the JSON/base64/UUID
  helper boundary can build without dragging Linux daemon dependencies into the
  app.

## Verification

```bash
tools/run_android_checks.sh
tools/run_android_checks.sh --docker
tools/run_android_checks.sh --host
tools/run_android_checks.sh --connected
```

`--connected` requires `adb devices` to show a device or emulator in the
`device` state. It installs and runs the instrumented native smoke on that
runtime; it is not part of ordinary CI.
