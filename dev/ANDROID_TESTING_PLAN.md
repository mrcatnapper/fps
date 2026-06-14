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
  `tools/run_android_checks.sh` builds the source-free Android base image and
  runs host Gradle checks inside it with the current workspace bind-mounted.
- `tools/run_android_checks.sh --host` runs JVM tests plus debug APK, release
  APK and instrumented-test APK assembly. It requires an Android SDK but no
  emulator. The release APK smoke verifies that debug-only native test hooks do
  not become production JNI exports.
- `tools/run_android_checks.sh --connected` runs the current instrumented native
  smoke on an already attached Android device or emulator.
- `tools/run_android_checks.sh --docker-managed-device` reuses existing Android
  Docker image tags by default; use `FPS_ANDROID_FORCE_DOCKER_REBUILD=1` only
  when intentionally rebuilding image contents.
- `Dockerfile.android` is intentionally a build/test image, not an emulator
  image. Its `android-gradle-base` stage installs SDK/NDK/vcpkg Android OpenSSL
  and prewarms Gradle dependencies without copying the full source tree. The
  source-containing final stage is opt-in only.
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
  It also covers inbound `CovertDatagramTransport` delivery back to the
  native-owned duplicated TUN fd, including missing-TUN/empty-payload rejects
  and fragment reassembly before exact write. It also includes a debug-only
  fake carrier lifecycle/enqueue smoke proving allowed packets route through
  the production `CovertDatagramTransport` path, plus an in-memory native
  Zero-RTT auth smoke that exercises shared auth/record/control codecs and
  emits a metadata-only encrypted TUN lease event through JNI.
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

## Product-Flow Testing Policy

The current Android scaffold has enough low-level smoke coverage for native
library loading, fd ownership, policy queues, fake carriers and passthrough
bridge bytes. New Android tests should now prefer product-shaped checks:

- one test should verify a meaningful lifecycle or data-flow property, not only
  that a helper returns the obvious value;
- isolated tests are still appropriate when they define a new failure contract
  before implementation, but they should feed into a product-flow test in the
  same increment;
- underlying-network DNS, socket protection and raw bridge startup should be
  checked as steps inside the coordinator/carrier-auth flow instead of as an
  ever-growing list of standalone blockers;
- emulator-managed tests should stay small, but their target should be
  production composition: protected socket, real bridge auth, lease, TUN attach
  and packet movement.

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
FPS_ANDROID_FORCE_DOCKER_REBUILD=1 tools/run_android_checks.sh --docker-managed-device
```

The helper ensures the source-free base image and emulator image exist, then
runs the managed-device task in a container with `/dev/kvm` passed through and
the current workspace bind-mounted. Existing default tags are used directly and
only missing images are built:

- `fps:android-ci-base`
- `fps:android-emulator-ci`

Avoid long-lived custom local tags for routine Android checks. They make it too
easy to build a second independent SDK/emulator image set and waste disk space.
Use `tools/run_android_checks.sh --clean-images` for allowlisted Android image
cleanup; by default it prunes dangling images and keeps useful Android cache
tags. Set `FPS_ANDROID_CLEAN_TAGS=1` only when intentionally resetting Android
images. Do not use broad Docker prunes as part of routine checks.

The emulator tag is intentionally large. It inherits the SDK/NDK/Gradle/vcpkg
Android OpenSSL base and adds the Android emulator plus the API 30 AOSP ATD
x86_64 system image. Keep it tagged when managed-device tests are part of the
local workflow; rebuilding it on every run wastes network, disk and time. Use
`FPS_ANDROID_FORCE_DOCKER_REBUILD=1` only when the image contents intentionally
changed. Do not create parallel emulator tags unless a specific experiment
needs them.
Keep this lane opt-in until repeated local/agent runs show it is stable enough
for scheduled CI.

AGP 9.0.1 currently exposes `ManagedVirtualDevice.testedAbi` in the public DSL
but does not propagate that value into the generated setup task. The build file
therefore also sets the setup task's `testedAbi` property through a narrow
reflection workaround. Remove that workaround once AGP forwards the DSL property
itself.

The repository has a manual-only GitHub Actions workflow named
`Android Emulator` for the same lane. It is not a required PR check; it exists
to validate the Docker-managed emulator setup in GitHub-hosted infrastructure
without putting emulator startup latency or KVM availability into the normal PR
path.

## VPN-Specific Product Scenarios

Prioritize these emulator/device scenarios in order:

1. **Headless coordinator lifecycle**
   - One JVM fake-backend test should drive the intended production sequence:
     start native executor, resolve through underlying network, prepare/protect
     raw fd, connect, start bridge, start app-owned cover client, apply lease,
     establish TUN, start pump and drain policy.
   - One managed-device smoke should validate the Android-only pieces of that
     same sequence with real `VpnService` fd ownership.

2. **Socket loop-prevention and underlying-network resolution**
   - Verify these as part of the coordinator path. The production FPS carrier
     must protect sockets before connect and must not rely on native resolver
     behavior after VPN activation.

3. **Split-tunnel UID policy**
   - Use the native pump's parsed TCP/UDP 5-tuple as the bridge to Android
     policy.
   - On API 29+, `ConnectivityManager.getConnectionOwnerUid(...)` can identify
     owners for connections associated with the active VPN. It may return
     `Process.INVALID_UID` or throw if unsupported/not active; FPS must keep the
     current fail-closed policy for those cases.

4. **Service revoke/stop lifecycle**
   - Current managed-device coverage exercises explicit stop and a debug
     `onRevoke()` path after full runtime startup.
   - Repeat this through the coordinator once native carrier/auth startup is no
     longer test-lease injected.

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
- It also covers native raw TCP carrier socket lifecycle through a local
  loopback server: native exposes the pre-connect fd, Kotlin has a chance to
  call the protect hook, native connects only after positive protection and
  closes the socket cleanly. Edge coverage includes complete-before-prepare and
  idempotent stop before prepare, after prepare and after connect.
- It covers the first native auth-core smoke without a real network carrier:
  client auth metadata is configured through JNI, invalid server public key
  encoding is rejected, a shared C++ Zero-RTT exchange returns an encrypted test
  TUN lease event and a tampered server accept reports failure without a lease.
  A protected raw carrier bridge smoke also opens a native-protected outbound
  socket, exposes a loopback local-cover listener, verifies TLS-record-shaped
  bytes pass through `TlsTcpCarrierSession` in both directions, then exercises
  real client-side Zero-RTT over that bridge with encrypted lease delivery and
  tampered server-accept failure.

## Next Android Runtime Steps

1. Keep the minimal foreground-service/status surface covered by JVM tests
   through a fake notifier/status sink and add device-level checks only where
   Android framework behavior is required.
2. Keep the first product carrier path HTTPS-only. WSS remains probe-support
   code and future external-carrier research; do not add a raw WSS internal
   carrier unless the Android product direction changes explicitly.
3. Extend the managed-device lane from fd/pump smoke to the smallest
   production-shaped HTTPS flow that requires Android framework behavior.
4. Keep managed-device CI manual/scheduled until repeated runs show it is
   stable enough for PR gating.
