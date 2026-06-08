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
- `tools/run_android_checks.sh --host` runs JVM tests plus debug APK and
  instrumented-test APK assembly. It requires an Android SDK but no emulator.
- `tools/run_android_checks.sh --connected` runs the current instrumented native
  smoke on an already attached Android device or emulator.
- `Dockerfile.android` is intentionally a build/test image, not an emulator
  image. It installs SDK/NDK/vcpkg Android OpenSSL and prewarms Gradle
  dependencies, but it does not contain system images or an emulator.
- `Dockerfile.android-emulator` extends the Android build image with the
  Android emulator and the API 30 x86_64 AOSP ATD system image. It is opt-in
  and requires host `/dev/kvm` passthrough.
- Current instrumented smoke validates native library loading, crypto/core
  smoke, IPv4 5-tuple parsing, native runtime handle lifecycle, executor
  start/stop, native-owned duplicate TUN fd attachment and the first native
  TUN pump skeleton against a pipe fd.

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
   - Add a Gradle Managed Device lane once the current native pump skeleton is
     stable. This lane should run a small instrumented test that exercises real
     `VpnService` preparation/establishment behavior, not just fake builders.
   - This lane must be opt-in until it has been run repeatedly without
     flakiness on the project VM.

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

Recommended command shape:

```sh
./gradlew :android:app:fpsApi30AtdDebugAndroidTest \
  -Pandroid.testoptions.manageddevices.emulator.gpu=swiftshader_indirect
```

Add a repository helper mode later:

```sh
tools/run_android_checks.sh --managed-device
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
```

The helper builds the base image first, builds the emulator image from that
local tag, then runs the managed-device task in a container with `/dev/kvm`
passed through. Keep this lane opt-in until repeated local/agent runs show it is
stable enough for scheduled CI.

## VPN-Specific Test Scenarios

Prioritize these emulator/device scenarios in order:

1. **Real `VpnService` preparation and fd establishment**
   - Test `VpnService.prepare(...)` behavior.
   - If user consent is needed, use UI Automator to accept the system dialog in
     the managed-device lane.
   - Verify that the service can establish the TUN fd after a test lease and
     that native snapshots report fd attachment plus pump running.

2. **Service revoke/stop lifecycle**
   - Exercise `onRevoke()` and explicit stop.
   - Assert the Kotlin controller closes the `ParcelFileDescriptor`, native
     pump stops and native executor stops without leaking handles.

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

## Near-Term Implementation Plan

1. Add Gradle Managed Device config for `fpsApi30Atd`.
2. Add `tools/run_android_checks.sh --managed-device` and
   `--docker-managed-device`.
3. Add `Dockerfile.android-emulator` as a child of `Dockerfile.android`.
4. Run the current instrumented smoke on the managed device manually.
5. If stable, add a small real `VpnService` consent/establish smoke with a
   test-only lease injection path.
6. Keep managed-device CI manual/scheduled until repeated local runs show it is
   stable enough for PR gating.
