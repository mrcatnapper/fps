# Android Testing Plan

This developer note records the Android emulator/device testing strategy for
FPS. It is a handoff artifact for agents and developers, not public operator
documentation.

## References Checked

- Android testing overview:
  <https://developer.android.com/training/testing>
- Android instrumented tests:
  <https://developer.android.com/training/testing/instrumented-tests>
- Gradle Managed Devices:
  <https://developer.android.com/studio/test/managed-devices>
- Instrumented test stability:
  <https://developer.android.com/training/testing/instrumented-tests/stability>
- UI Automator:
  <https://developer.android.com/training/testing/other-components/ui-automator>
- `VpnService` API, especially `prepare`, `onRevoke` and `protect`:
  <https://developer.android.com/reference/android/net/VpnService>
- `ConnectivityManager.getConnectionOwnerUid`:
  <https://developer.android.com/reference/android/net/ConnectivityManager#getConnectionOwnerUid(int,%20java.net.InetSocketAddress,%20java.net.InetSocketAddress)>

## Current Baseline

- Default Android verification is still Docker/JVM-first:
  `tools/run_android_checks.sh` builds the Android CI image and runs host
  Gradle checks inside it.
- `tools/run_android_checks.sh --host` runs JVM tests plus debug APK, release
  APK and instrumented-test APK assembly. It requires an Android SDK but no
  emulator. The release APK smoke guards the production variant after
  debug-only test hooks are added.
- `tools/run_android_checks.sh --connected` runs the current instrumented native
  smoke on an already attached Android device or emulator.
- `tools/run_android_checks.sh --docker-managed-device` can now reuse existing
  Android Docker image tags with `FPS_ANDROID_REUSE_DOCKER_IMAGE=1`; use this
  for fast reruns after the base/emulator images have already been built.
- `Dockerfile.android` is intentionally a build/test image, not an emulator
  image. It installs SDK/NDK/vcpkg Android OpenSSL and prewarms Gradle
  dependencies in the source-free `android-gradle-base` stage, before the full
  source `COPY`.
- `Dockerfile.android-emulator` extends the Android build image with the
  Android emulator and the API 30 x86_64 AOSP ATD system image. It is opt-in
  and requires host `/dev/kvm` passthrough. It must inherit from the
  source-free base image, not from the final source-copied Android CI image, so
  ordinary source edits do not invalidate emulator/system-image layers.
- Current instrumented smoke validates native library loading, crypto/core
  smoke, IPv4 5-tuple parsing, native runtime handle lifecycle, executor
  start/stop, native-owned duplicate TUN fd attachment and the first native
  TUN pump skeleton against a pipe fd, including policy metadata drain and
  allow/drop completion, default no-carrier enqueue rejection and a capture
  sink check proving exact native-owned packet bytes reach the outbound seam.
  The managed-device lane also includes a
  debug-only real `VpnService.prepare(...)` / `VpnService.Builder.establish()`
  smoke that requests consent through UI Automator when needed, verifies a real
  TUN fd/MTU, routes the fd through `HeadlessNativeVpnRuntime`, verifies native
  `owned_duplicate` attachment plus pump startup and closes it through explicit
  stop/revoke paths.

## Testing Layers For FPS Android

Keep the test pyramid biased toward fast deterministic tests. Use emulators only
for contracts that require real Android framework behavior.

1. **Linux/C++ core tests**
   - Protocol, crypto, TLS record parsing, covert datagrams, shaper and TUN
     adapters stay covered by existing Boost.Test, integration tests, fuzzing
     and Docker/TUN soaks.
   - Android should not duplicate those protocol tests in Kotlin.

2. **JVM Android tests**
   - Keep using ordinary JVM tests for profile parsing, carrier-probe planning,
     split-tunnel policy decisions, fake platform hooks, `VpnService.Builder`
     planning and Kotlin/native facade lifecycle.
   - These tests should remain the fastest Android signal and should run in
     every normal Android Docker check.

3. **Instrumented native smoke**
   - Use `src/androidTest` for JNI/native behavior that must run on Android:
     loading `libfps_android_native.so`, Android ABI linkage, native fd
     duplication, executor lifecycle and native pump behavior against file
     descriptors.
   - Keep this smoke headless: no GUI assertions and no external network unless
     a later test explicitly declares that dependency.

4. **Emulator-managed VPN smoke**
   - The Gradle Managed Device lane runs a small instrumented test that
     exercises real `VpnService` preparation/establishment behavior, not just
     fake builders.
   - This lane must remain opt-in until it has been run repeatedly without
     flakiness on the project VM and GitHub-hosted runners.

5. **Real device/manual release smoke**
   - Keep a short manual or agent-run checklist for device-specific behavior
     that emulators do not faithfully cover: battery/background restrictions,
     vendor VPN quirks, always-on/lockdown behavior and network switching.

## Gradle Managed Device Direction

Use Gradle Managed Devices before custom emulator scripts. The Android Gradle
Plugin can create, start, clean up and report managed devices, and official
docs recommend this path for more consistent instrumented tests.

First target:

- name: `fpsApi30Atd`
- device: `Pixel 2`
- API: 30
- system image: `aosp-atd`
- purpose: fast headless instrumented smoke

Future compatibility target:

- name: `fpsApi36Aosp`
- device: a small phone profile
- API: 36
- system image: `aosp`
- purpose: target-SDK compatibility smoke after the API 30 ATD lane is stable

The implemented managed-device command shape is:

```sh
tools/run_android_checks.sh --managed-device
```

For Docker-isolated runs with the emulator image:

```sh
tools/run_android_checks.sh --docker-managed-device
```

Do not include this mode in ordinary PR CI immediately. Start with
`workflow_dispatch` or local/agent runs, then consider scheduled CI after the
lane is stable.

## Docker And Emulator Boundary

Keep two Android images:

- `Dockerfile.android`: the normal SDK/NDK/vcpkg/Gradle build image used by
  ordinary Android CI and host/JVM checks.
- `Dockerfile.android-emulator`: a heavier child image that adds emulator
  runtime dependencies plus `emulator`, `platforms;android-30` and
  `system-images;android-30;aosp_atd;x86_64`.

This keeps ordinary Android CI fast and cacheable while still making
post-JVM/runtime validation reproducible without depending on host SDK paths.
The emulator image must be launched with `/dev/kvm`; software emulation is not a
supported FPS test baseline because it is too slow and can hide lifecycle and
timing issues.

Use the repository helper:

```sh
tools/run_android_checks.sh --docker-managed-device
FPS_ANDROID_REUSE_DOCKER_IMAGE=1 tools/run_android_checks.sh --docker-managed-device
```

The helper builds the base image first, builds the emulator image from that
local tag, then runs the managed-device task in a container with `/dev/kvm`
passed through and the current workspace bind-mounted. The base image is built
from the source-free `android-gradle-base` target. With
`FPS_ANDROID_REUSE_DOCKER_IMAGE=1`, existing image tags are used directly and
only missing images are built. Keep this lane opt-in until repeated local/agent
runs show it is stable enough for scheduled CI.

The repository has a manual-only GitHub Actions workflow named
`Android Emulator` for the same lane. It is not a required PR check; it exists
to validate the Docker-managed emulator setup in GitHub-hosted infrastructure
without putting emulator startup latency or KVM availability into the normal PR
path.

## VPN-Specific Test Scenarios

Prioritize these emulator/device scenarios in order:

1. **Real `VpnService` preparation and fd establishment**
   - Current managed-device coverage uses a debug-only harness activity/service
     to test `VpnService.prepare(...)` behavior, accept the system dialog with
     UI Automator when needed, establish a real TUN fd after a test lease and
     close it.
   - Current managed-device coverage also routes that real fd through the full
     `HeadlessNativeVpnRuntime` attach path, starts the native TUN pump and
     verifies clean cleanup. Pipe-fd native smoke separately verifies metadata
     drain, allow/drop completion, no-carrier enqueue rejection and exact packet
     delivery into a test capture sink.

2. **Service revoke/stop lifecycle**
   - Current managed-device coverage exercises explicit stop and a debug
     `onRevoke()` path after full runtime startup.
   - Future coverage should repeat this through the production service once
     native carrier/auth startup is no longer test-lease injected.

3. **Socket loop-prevention**
   - Verify app-owned Java sockets call `VpnService.protect(Socket)` before
     connect.
   - Later, verify native sockets call `protect(fd)` before connect. The
     production FPS carrier must never loop into its own VPN routes.

4. **Underlying-network resolution**
   - Confirm Android code resolves carrier/FPS endpoints through the selected
     underlying `Network`, then passes resolved endpoints to native code.
   - Do not trust native resolver behavior after VPN activation.

5. **Split-tunnel UID policy**
   - Use the native pump's parsed TCP/UDP 5-tuple as the bridge to Android
     policy.
   - On API 29+, `ConnectivityManager.getConnectionOwnerUid(...)` can identify
     owners for connections associated with the active VPN. It may return
     `Process.INVALID_UID` or throw if unsupported/not active; FPS must keep the
     current fail-closed policy for those cases.

6. **Packet pump and policy integration**
   - Once the policy bridge exists, feed controlled TCP/UDP traffic through the
     emulator VPN.
   - Assert counters distinguish accepted packets, malformed packets,
     unsupported protocols and policy drops.

7. **Controlled native carrier/auth smoke**
   - Only after TUN and policy behavior is stable, add native raw TLS/TCP carrier
     auth against a local test origin/server.
   - Keep this as a small smoke first; full Android VPN E2E should come after
     reconnect/status behavior is observable.

## Stability Rules

- Avoid arbitrary `sleep` in instrumented tests. Wait for explicit conditions:
  state transitions, snapshot counters, service state or visible system dialog
  elements.
- Keep each emulator test small. One test should validate one Android contract,
  not the whole VPN product.
- Use UI Automator only for system UI that cannot be driven directly through app
  APIs, such as VPN consent dialogs.
- Retries are acceptable only as infrastructure protection for emulator/device
  disconnects. A functional flake should be fixed, not hidden.
- All Android test snapshots/logs must remain metadata-only: no UUIDs, keys,
  ClientID, raw packets or payload bytes.

## Completed Baseline

- Gradle Managed Device config exists for `fpsApi30Atd`.
- `tools/run_android_checks.sh` supports `--managed-device` and
  `--docker-managed-device`.
- `Dockerfile.android-emulator` extends the source-free
  `android-gradle-base` stage from `Dockerfile.android`, so ordinary source
  edits do not invalidate the emulator/system-image layers.
- A manual-only GitHub Actions workflow can run the Docker-managed emulator
  lane.
- The managed-device lane has been run locally with `/dev/kvm` and covers both
  the native/JNI smoke and debug-only real `VpnService`
  consent/establish/full-runtime fd attach/pump/stop/revoke smoke.

## Next Android Runtime Steps

1. Add socket loop-prevention coverage for app-owned Java sockets first, then
   native carrier sockets.
2. Add underlying-network DNS coverage before enabling native carrier connect.
3. Add split-tunnel UID policy integration over emulator VPN traffic once the
   real-fd runtime path is stable.
4. Wire the native outbound packet seam into real native carrier enqueue.
5. Keep managed-device CI manual/scheduled until repeated runs show it is
   stable enough for PR gating.
