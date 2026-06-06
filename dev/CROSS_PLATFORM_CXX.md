# Cross-Platform C++ Practices

This note records the current rules for keeping FPS C++ code reusable by the
Linux daemon and the Android client.

## Diagnose Before Disabling

When Android cross-builds fail, first identify the class of dependency:

- header-only library;
- compiled library with a link/runtime artifact;
- host-system include leakage;
- OS/runtime API mismatch.

Do not disable a useful dependency just because the first Android build cannot
find it. Prefer the narrowest fix that preserves the shared contract.

## Header-Only Boost

Boost.Describe, Boost.MP11 and Boost.Endian are header-only for current FPS use.
They are acceptable in Android native code when provided through a clean Boost
header root.

Do not add `/usr/include` to an Android/NDK target. It exposes host glibc
headers to the cross-compiler and can override NDK sysroot headers through
normal include lookup or `#include_next`.

The Android scaffold uses `FPS_ANDROID_BOOST_DIR`, defaulting to
`/usr/include/boost`, and CMake creates an isolated generated include root that
contains only `boost/`. Override it when using a vendored or separately
installed Boost header set:

```bash
./gradlew :android:app:assembleDebug \
  -Pandroid.injected.cmake.configure.arguments=-DFPS_ANDROID_BOOST_DIR=/path/to/boost
```

## Compiled Dependencies

Compiled dependencies are different from header-only Boost. Boost.Log,
Boost.JSON where linked, Boost.System and OpenSSL must be cross-built for
Android or hidden behind narrower platform interfaces before broad native-core
linkage.

Rules:

- keep product code behind project facades (`FPS_LOG_*`, crypto helpers,
  platform runtime hooks);
- put platform dispatch in one small header/source boundary, not at call sites;
- keep Linux daemon behavior as the default path;
- add Android smoke builds as soon as a boundary is introduced.

## Logging

Operational code must use `FPS_LOG_*`, never direct Boost.Log calls. On Linux,
the facade uses Boost.Log. On Android, `FPS_LOG_WITH_SEVERITY` resolves to a
small stream-style `__android_log_print` backend, so Android code can include
the logging facade without linking Boost.Log.

The Android backend must keep Boost.Log's important lazy behavior: if the
runtime severity threshold disables a message, the stream expression after
`FPS_LOG_*` must not be evaluated or formatted. This matters for expensive
structured log values and for avoiding accidental payload formatting in cold
paths.

If Android later needs configurable log levels or structured log sinks, add an
Android-specific implementation behind the same facade instead of changing
callers.
