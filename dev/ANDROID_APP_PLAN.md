# Android Client Plan

This developer note records the current Android client direction. It is a
handoff artifact, not operator documentation.

## Current Increment

- Use Kotlin for the Android application layer and C++20/NDK for FPS native
  protocol code.
- Add a minimal Android app module that can be built from the command line
  without Android Studio.
- Build a small JNI native library for `arm64-v8a` and `x86_64` that reuses the
  existing FPS IPv4 TCP/UDP 5-tuple parser. This directly supports Android
  split-tunnel UID policy and avoids linking Linux runtime code.
- Keep the first scaffold headless: no real VPN lifecycle, GUI, emulator or
  carrier manager yet.

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
export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"
```

## Accepted Runtime Direction

- The first Android beta uses app-owned carrier sessions. The app opens and
  maintains HTTPS/WSS carrier traffic itself.
- Carrier requests are configured at the Android layer: for example a periodic
  HTTPS GET or a WSS stream probe. This does not require FPS protocol changes.
- Carrier sockets must be protected with `VpnService.protect(fd)` before
  `connect`.
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

## Next Native Dependency Work

The current Android native smoke intentionally does not link the broad
`fps_core` target. Full core linkage still needs a deliberate dependency
strategy for Android:

- Header-only Boost.Describe, Boost.MP11 and Boost.Endian are usable in Android
  native code when exposed through an isolated Boost header root. Do not add
  `/usr/include` to Android targets.
- Boost.JSON, Boost.System and OpenSSL must either be cross-built
  reproducibly for Android or avoided behind narrower Android-facing targets.
- Boost.Log stays behind `FPS_LOG_*`; Android uses a small `__android_log_print`
  stream backend for the logging macro instead of linking Boost.Log.
- Profile parsing should be reused from C++ only after the JSON/base64/UUID
  helper boundary can build without dragging Linux daemon dependencies into the
  app.

## Verification

```bash
./gradlew :android:app:assembleDebug
./gradlew :android:app:testDebugUnitTest
```
