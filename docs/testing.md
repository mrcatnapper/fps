# FPS Testing And Quality Workflow

The regression baseline is meant to catch changes in TLS passthrough,
Zero-RTT upgrade, classified FPS records, the generic covert datagram transport,
the Linux TUN adapter, carrier-pool scheduling, fragmentation, shaper budgeting,
CLI/config behavior and logging safety.

## Quick Local Suite

```sh
python3 -m py_compile tests/integration/*.py tools/*.py
bash -n tools/*.sh docker/*.sh
cmake -S . -B build
cmake --build build -j 2
ctest --test-dir build --output-on-failure
ctest --test-dir build -L local --output-on-failure
```

The root CMake project defaults to `CMAKE_BUILD_TYPE=Release` for single-config
generators and uses `-O2 -DNDEBUG` for GNU/Clang Release builds. Developers who
need debug symbols or sanitizer-specific flags should pass
`-DCMAKE_BUILD_TYPE=Debug`, `RelWithDebInfo`, or explicit `CMAKE_CXX_FLAGS`.

Real TUN checks require `sudo`/`CAP_NET_ADMIN` and stay opt-in:

```sh
cmake -S . -B cmake-build-tun -DFPS_ENABLE_TUN_TESTS=ON
cmake --build cmake-build-tun -j 2
sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure
```

## Quality And Sanitizers

Use the repeatable non-sudo quality wrapper:

```sh
tools/run_quality_checks.sh --all
```

The wrapper keeps separate build directories and does not run opt-in root/TUN or
pcap tests. Modes:

- `--python`: `py_compile` for integration scripts/tools and an import check for
  pinned Python runtime dependencies.
- `--clang`: `clang++-20` Debug build plus `ctest -L local`.
- `--sanitizers`: `clang++-20` Debug build with ASan+UBSan plus
  `ctest -L local`.
- `--valgrind`: Debug build plus Valgrind over `fps_unit_tests`.
- `--coverage`: `clang++-20` Debug build with
  `-fprofile-instr-generate -fcoverage-mapping`, local CTest,
  `llvm-profdata merge`, `llvm-cov report/export/show`.
- `--fuzz`: `clang++-20` libFuzzer build with ASan+UBSan and a bounded smoke run
  over FPS-owned parsers/codecs.
- `--docker`: opt-in Docker build/smoke and `docker compose config` checks.
- `--soak-smoke`: opt-in short Docker/TUN resilience run with leased clients,
  carrier loss/recovery and status-counter assertions.

Useful environment variables:

- `FPS_JOBS=4`: build parallelism.
- `FPS_DOCKER_IMAGE=fps:local`: Docker tag for `--docker`.
- `FPS_DOCKERFILE=Dockerfile|Dockerfile.alpine`: Dockerfile used by
  `--docker`, default `Dockerfile`.
- `FPS_DOCKER_COMPILER=gcc|clang`: compiler used by the Docker builder stage,
  default `gcc`; `Dockerfile.alpine` currently supports `gcc` only.
- `FPS_DOCKER_BUILDKIT=0`: disable `docker buildx build --load` for
  `--docker`; the wrapper uses buildx automatically when it is available so
  unrelated CI-only Dockerfile stages are skipped during product image builds.
- `FPS_DOCKER_SUDO=1`: run Docker checks through `sudo -n docker`.
- `FPS_DOCKER_SOAK_BUILD=0`: reuse an existing image for `--soak-smoke`;
  default is to build `FPS_DOCKER_IMAGE`.
- `FPS_DOCKER_SOAK_DURATION=60`: short resilience smoke duration.
- `FPS_DOCKER_SOAK_BANDWIDTH=500K` and `FPS_DOCKER_SOAK_LENGTH=512`: UDP probe
  settings for the resilience smoke.
- `FPS_DOCKER_SOAK_STRESS=0`: skip the write-queue pressure phase.
- `FPS_DOCKER_SOAK_REQUIRE_BACKPRESSURE=1`: fail unless the pressure phase
  observes `write_queue_full`; use with a deliberately low
  `--write-queue-bytes` manual run. This is a diagnostic mode, not the default
  beta soak gate, because Docker-level queue saturation is timing/topology
  dependent.
- `FPS_DOCKER_SOAK_WITH_SOCKS=1`: include the derivative Dante proxy overlay in
  the resilience smoke.
- `CLANG_CXX=/path/to/clang++`: override `clang++-20`.
- `LLVM_COV=/path/to/llvm-cov` and
  `LLVM_PROFDATA=/path/to/llvm-profdata`: override coverage tools.
- `FPS_COVERAGE_MIN_LINES=70` and `FPS_COVERAGE_MIN_FUNCTIONS=80`: total
  coverage gates.
- `FPS_FUZZ_RUNS=256`: libFuzzer iterations per smoke target.
- `FPS_FUZZ_SECONDS=N`: optional wall-clock bound for each fuzz target.

Local non-Docker Python runtime dependencies are pinned in
`requirements-runtime.txt`.

Android Docker helpers reuse existing tagged Android images by default and build
only missing tags. Use `FPS_ANDROID_FORCE_DOCKER_REBUILD=1` after Dockerfile
layer, apt/sdk package or base-image changes when the existing tag must be
rebuilt. A forced base-image rebuild also removes the stale
`fps:android-emulator-ci` tag, because the emulator image extends that base and
must be recreated from the new parent. Android cleanup is an explicit action
through `tools/run_android_checks.sh --clean-images`.

If you only need to inspect or smoke-test an already built image, run it
directly instead of rebuilding:

```sh
docker run --rm fps:local fps_client --help
docker run --rm -v "$PWD:/workspaces" -w /workspaces \
  fps:android-ci-base tools/run_android_checks.sh --host
```

The bind-mounted form uses the current working tree while reusing the toolchain
from the image. This is useful for quick checks, but remember that generated
build outputs will be written to the mounted workspace unless the command
redirects them elsewhere.

Android Docker checks have the same reuse path by default:

```sh
tools/run_android_checks.sh --docker
tools/run_android_checks.sh --docker-managed-device
FPS_ANDROID_FORCE_DOCKER_REBUILD=1 tools/run_android_checks.sh --docker-managed-device
```

To reclaim Android image space, use the allowlisted cleanup mode instead of a
global Docker prune. By default it only prunes dangling images and preserves
useful Android cache tags:

```sh
FPS_ANDROID_CLEAN_DRY_RUN=1 tools/run_android_checks.sh --clean-images
tools/run_android_checks.sh --clean-images
FPS_ANDROID_CLEAN_TAGS=1 tools/run_android_checks.sh --clean-images
```

## Android Bootstrap Checks

The Android client scaffold is command-line only; Android Studio is not
required. The preferred reproducible path is Docker:

```sh
tools/run_android_checks.sh
tools/run_android_checks.sh --docker
```

Outside Docker, no arguments default to `--docker`. Inside `Dockerfile.android`,
the container sets `FPS_ANDROID_DOCKER=1`, so no arguments default to the host
Gradle checks against the SDK installed in the image. This keeps ordinary
Android verification independent from host SDK/NDK/vcpkg paths.

`Dockerfile.android` installs JDK 21, Android command-line tools, platform 36,
build-tools 36.0.0, NDK 28.2, SDK CMake and Android OpenSSL through vcpkg. This
image is a build/test image, not the FPS product runtime image. It deliberately
does not include an emulator. Emulator execution uses the separate
`Dockerfile.android-emulator` child image.

Host SDK checks remain available for developers who already have a local SDK.
Install the Android SDK under `/opt/android-sdk` and export:

```sh
export ANDROID_HOME=/opt/android-sdk
export ANDROID_SDK_ROOT=/opt/android-sdk
export ANDROID_NDK_HOME=/opt/android-sdk/ndk/28.2.13676358
export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"
```

Required SDK packages for the current scaffold:

```sh
sdkmanager "platform-tools" "platforms;android-36" \
  "build-tools;36.0.0" "ndk;28.2.13676358" "cmake;3.22.1"
```

Install Android OpenSSL through the existing vcpkg checkout:

```sh
ANDROID_NDK_HOME=/opt/android-sdk/ndk/28.2.13676358 \
  /opt/vcpkg/vcpkg install openssl:arm64-android openssl:x64-android
```

This vcpkg usage is intentionally limited to Android OpenSSL. Linux, Docker,
Alpine and Boost dependency paths are not managed by vcpkg. Dockerized Android
builds remove vcpkg checkout, buildtree, download and package work directories
after installing the Android OpenSSL triplets; the reusable image keeps only
the installed triplet outputs and the minimal vcpkg metadata needed for
inspection.

Android Kotlin code should use Android/Kotlin/JVM libraries for common parsing
and encoding tasks. Client profile parsing uses Android's `org.json`; JVM unit
tests get the same API through a test-only dependency so they stay headless.

`Dockerfile.android` prewarms Gradle in the source-free `android-gradle-base`
stage: it copies only the Gradle wrapper, build files and minimal Android
project metadata, sets a stable `GRADLE_USER_HOME` and runs a
dependency-resolution task. Ordinary `--docker` checks build this target as
`fps:android-ci-base` and bind-mount the current workspace into the container,
so source edits do not create a new heavy source-copied image. The final
source-containing `ci` stage remains available only for explicit image-shape
experiments.

`fps:android-emulator-ci` is expected to be much larger than
`fps:android-ci-base`: it includes the Android emulator binary, API 30 platform
metadata and the `system-images;android-30;aosp_atd;x86_64` managed-device
image. The large layer is intentional and is used only by the opt-in
`--docker-managed-device` lane. Keep the default `fps:android-emulator-ci` tag
around when managed-device tests are part of the local workflow; subsequent
managed-device runs reuse it by default. Avoid custom emulator tags unless a
specific experiment needs them.

Run host Android checks through the repository helper or the Gradle wrapper, not
the old system Gradle:

```sh
tools/run_android_checks.sh --host
./gradlew :android:app:tasks --all
./gradlew :android:app:testDebugUnitTest
./gradlew :android:app:assembleDebug
./gradlew :android:app:assembleRelease
./gradlew :android:app:assembleDebugAndroidTest
```

`tools/run_android_checks.sh --host` runs JVM unit tests, assembles debug and
release APKs, and assembles the instrumented test APK. It does not require an
emulator. The release APK smoke checks the release native library's exported
symbols so debug-only JNI test hooks cannot become an accidental production
dependency. The JVM tests cover Android client-profile
parsing, private profile persistence/start-from-stored-profile behavior,
fail-closed split-tunnel policy, the headless VPN runtime state machine and the
headless carrier probe runner with fake platform hooks/transports. They also
cover profile-driven carrier probe planning,
underlying-network endpoint resolution, socket-protection ordering,
reconnect/backoff behavior, UID allowlist decisions and the live OkHttp
HTTPS/WSS carrier probe transport factory through MockWebServer. They also
cover Android TUN plan generation, `VpnService.Builder` call sequencing,
idempotent TUN fd close ownership through fake builders and the Kotlin
`FpsNativeRuntime` wrapper with a fake native backend. They also cover the
headless Kotlin/native lifecycle bridge that establishes TUN after lease
delivery, starts the native executor lifecycle and duplicates the descriptor
into native-owned runtime state. They also exercise the Kotlin/JNI-facing TUN
pump lifecycle through fake backends: pump startup requires a started runtime
and attached TUN fd, lease-triggered attach starts the pump, and stop/close are
idempotent. Current JVM tests also cover the service-owned Android coordinated
runner: native raw carrier start, local cover-client start, lease/TUN/policy
ticks, transient failure backoff/retry, VPN-permission terminal state and
metadata-only snapshots. A pure JVM service-runtime owner test covers
start/restart, factory-failure preservation of the active runner, idempotent
stop and stopped snapshots without Robolectric or Android framework classes.
They assert that lease-triggered TUN startup requires `tun.enabled=true` and
that snapshots report only non-secret state/TUN/carrier-probe/native metadata.
These checks still do not require an emulator or a real Android `VpnService`
instance.

To execute the native runtime smoke on an attached device or emulator:

```sh
adb devices
tools/run_android_checks.sh --connected
```

The connected check runs `:android:app:connectedDebugAndroidTest`, loads
`libfps_android_native.so` on the target, calls the JNI `nativeVersion()` and
`nativeCoreSmoke()` paths, verifies a native IPv4 TCP tuple parse fixture and
checks the JNI native runtime handle/snapshot, executor start/stop/no-op command
and native-owned duplicate TUN fd API. It also starts the native TUN pump
skeleton against a pipe fd, writes one valid IPv4 UDP packet plus one malformed
byte sequence, drains bounded policy metadata, completes allow/drop decisions
and verifies read/parse/drop/policy counters. The debug VPN harness also drives
a real `VpnService` fd through `HeadlessNativeVpnRuntime`, verifies native
`owned_duplicate` attachment plus pump startup and covers explicit
stop/debug-revoke cleanup. The native smoke also covers the first raw carrier
socket lifecycle: native opens a TCP socket, exposes the pre-connect fd to
Kotlin for `VpnService.protect(fd)`, aborts cleanly when protection is denied,
connects to a loopback TCP server when protection succeeds and closes the socket
without leaving carrier state active. It also starts the first protected raw
carrier bridge: native exposes a loopback local-cover listener, accepts a local
TCP client and verifies TLS-record-shaped bytes pass through the shared
`TlsTcpCarrierSession` to a loopback remote endpoint and back. It also verifies
real client-side Zero-RTT over that bridge: encrypted server-accept lease
delivery succeeds and a tampered server accept reports failure without a lease.
The smoke also injects inbound opaque datagrams through the shared
`CovertDatagramTransport` path and verifies exact writes to a duplicated TUN fd,
including missing-TUN/empty-payload rejects and fragmented datagram reassembly
before write. It is opt-in because it requires a real Android runtime.

For reproducible post-JVM checks without relying on host SDK paths, use the
Docker-managed emulator lane:

```sh
ls -l /dev/kvm
tools/run_android_checks.sh --docker-managed-device
```

This ensures `fps:android-ci-base` and `fps:android-emulator-ci` exist, building
only missing tags by default. The base comes from the source-free
`android-gradle-base` target and the emulator image comes from
`Dockerfile.android-emulator` built on that local base. The emulator image adds
Android's `emulator` package,
`platforms;android-30` and
`system-images;android-30;aosp_atd;x86_64`, then runs the Gradle Managed Device
task `:android:app:fpsApi30AtdDebugAndroidTest` in a container with `/dev/kvm`
passed through and the current workspace bind-mounted at `/workspaces`. The
bind mount is intentional: source edits do not invalidate the heavy
emulator/system-image layers. The managed-device lane runs the same
instrumented native smoke as `--connected` and also exercises debug-only real
`VpnService.prepare(...)` / `VpnService.Builder.establish()` coverage. The VPN
smoke requests consent through the system dialog when needed, verifies a real
TUN fd, routes that fd through `HeadlessNativeVpnRuntime`, starts the native TUN
pump and closes it through explicit stop/debug-revoke paths. It also includes a
small local product-flow smoke for the service-owned coordinator: protected raw
socket connect from a stored profile, native TLS/TCP bridge auth, encrypted
lease delivery, real TUN fd attach and native pump startup. This lane is opt-in
and is not part of ordinary PR CI until repeated local runs prove it stable.

GitHub Actions also has a manual-only `Android Emulator` workflow that runs the
same `--docker-managed-device` command on demand. It is intentionally not a
required PR check: emulator availability and startup latency are still treated
as infrastructure variables until repeated scheduled/manual runs prove stable.

The current Android scaffold builds `fps_android_native` for `arm64-v8a` and
`x86_64`, while the Kotlin layer parses client JSON/`fps://v1` profiles, models
the VPN startup state machine and provides an OkHttp-backed HTTPS/WSS carrier
probe factory for app-owned keepalive/probe sockets. OkHttp probes are not FPS
wire carriers; real FPS carrier traffic must use native raw TCP/TLS stream
handling. The first raw carrier bridge now reaches `TlsTcpCarrierSession` over
a protected raw socket plus a local loopback cover socket with real client-side
Zero-RTT and encrypted lease delivery. The Kotlin layer also includes a raw
HTTPS local cover client: it connects to the native loopback bridge, wraps that
socket in TLS using the configured carrier origin hostname and sends
HTTP/1.1 keep-alive GETs while preserving raw TLS bytes for the native bridge.
The Android carrier profile can bound per-response draining with
`max_response_bytes`; JVM tests cover oversized responses, chunked responses and
close-before-next-keepalive behavior.
Android profile tests also cover inline static shaper CDF parsing and rejection
of file-based `shaper.profile_file`; JNI/native smoke covers configuring the
shared native shaper from primitive CDF arrays.
OkHttp remains probe/support code, not the FPS wire carrier. The scaffold also
has a lease-triggered `VpnService.Builder` TUN fd ownership layer, tested with
fake builders and a real debug `VpnService` fd routed through the Kotlin/native
runtime bridge. It remains guarded by explicit `tun.enabled=true` profile
intent. The native smoke reuses the FPS IPv4 TCP/UDP 5-tuple parser and links
a reusable native core smoke library built from protocol codec/crypto, generic
covert datagram transport and TLS/TCP carrier sources. The smoke intentionally
excludes Linux relay/config/CLI, Linux TUN device code, Boost.Log and
Boost.JSON-heavy operator paths.
JVM tests also cover the first headless coordinator that composes native start,
underlying-network DNS, protected raw socket connect, bridge start, local
cover-client startup, encrypted lease handling, TUN attach/pump and split-tunnel
policy drain as one fail-closed product flow.

Header-only Boost.Describe/MP11/Endian are used through an isolated Boost header
root, defaulting to `/usr/include/boost`. Do not add `/usr/include` directly to
Android CMake targets; that leaks host libc headers into the NDK sysroot. To
use another Boost header installation, pass:

```sh
./gradlew :android:app:assembleDebug \
  -Pandroid.injected.cmake.configure.arguments=-DFPS_ANDROID_BOOST_DIR=/path/to/boost
```

Boost.Asio/Boost.System are used header-only in this Android target through
`BOOST_ERROR_CODE_HEADER_ONLY` and `BOOST_SYSTEM_NO_DEPRECATED`. OpenSSL is
linked from the Android vcpkg triplet as `libcrypto.a`.

Optional native dependency sanity check:

```sh
readelf -d android/app/build/intermediates/merged_native_libs/debug/mergeDebugNativeLibs/out/lib/arm64-v8a/libfps_android_native.so \
  | grep -E 'NEEDED|RUNPATH|RPATH'
```

The output should reference Android runtime libraries such as `liblog.so`,
`libm.so`, `libdl.so` and `libc.so`, not host paths under `/usr/lib`.

## GitHub Actions CI

The repository defines four GitHub Actions workflow files:

- `CI`: runs on pull requests, pushes to `main` and manual dispatch. It covers
  `ubuntu-24.04 x gcc/clang` local builds/tests through the repository
  `Dockerfile` `ci` stage, Docker build/runtime smoke for Ubuntu GCC/clang and
  Alpine GCC images, and the Android Docker build/unit/APK assembly smoke.
- `Quality`: runs on schedule and manual dispatch. It executes
  `tools/run_quality_checks.sh --all` inside the same `Dockerfile` `ci` stage,
  including clang-20, ASan/UBSan, Valgrind, llvm-cov and bounded libFuzzer
  smoke.
- `Android Emulator`: runs on manual dispatch only. It executes the
  Docker-managed Gradle Managed Device smoke through `/dev/kvm` and remains
  outside the required PR checks.
- `Publish Images`: runs on manual dispatch only. It first runs the same Docker
  runtime smoke, then can publish Ubuntu and Alpine images to GHCR when
  `publish=true`. With the default `publish=false`, it is a build-only
  deployment dry run.

GitHub also shows platform-managed workflows such as Pages deployment or
Dependency Graph when those repository features are enabled. They are not part
of the FPS test matrix.

The `ci` stage keeps package installation in the repository Dockerfile instead
of duplicating Boost/OpenSSL/LLVM package lists in workflow YAML. It is not part
of the product runtime image.

Workflow actions use current Node 24 based major versions. GitHub-hosted runners
already satisfy this requirement; self-hosted runners must be at least Actions
Runner `v2.327.1`.

The local CI test image can be repeated manually with:

```sh
docker build --target ci \
  --build-arg FPS_COMPILER=gcc -t fps:ci-local-gcc .
docker run --rm -e FPS_JOBS=2 fps:ci-local-gcc
```

If Docker Buildx is available, prefer `docker buildx build --load --target ci`
for the first command; it skips unrelated product stages more reliably.

The CI matrix intentionally excludes root/TUN, pcap and long Docker soak tests.
Those remain opt-in operator checks until they are safe and stable enough for a
dedicated privileged runner.

Repository operations are intentionally conservative: branch protection keeps
`main` behind pull requests and required CI checks, image publication stays
manual, and release artifacts should be scanned for accidental secrets before
they are shared.

The publish workflow writes to GitHub Container Registry with only:

```yaml
permissions:
  contents: read
  packages: write
```

It publishes explicit tags only. For `image_tag=v0.1.0-beta.1`, expected tags
are:

```text
ghcr.io/OWNER/fps:v0.1.0-beta.1
ghcr.io/OWNER/fps:v0.1.0-beta.1-alpine
```

The unsuffixed tag points to the Ubuntu image. Alpine is always explicit. When
`image_tag` is omitted, the workflow uses the current short commit SHA in the
same two-tag shape. Do not publish `latest` until public release policy is
defined.

Image publication disables Buildx provenance and SBOM attestations
(`provenance: false`, `sbom: false`) to avoid extra untagged OCI package
versions in GHCR. Supply-chain attestations and image signing are future release
policy items, not implicit side effects of the publish workflow.

Alpine Docker smoke can be repeated locally with:

```sh
FPS_DOCKER_SUDO=1 FPS_DOCKERFILE=Dockerfile.alpine \
  FPS_DOCKER_IMAGE=fps:alpine tools/run_quality_checks.sh --docker
```

The Docker smoke includes a generated UUID/server-key `--check-config` pass.
That check is required for Alpine release candidates because Alpine/OpenSSL
runtime differences can otherwise break UUID-derived Zero-RTT auth while basic
`--help` commands still work.

## Docker Product Simulations

One-client TUN UDP simulation:

```sh
FPS_DOCKER_SUDO=1 tools/docker_tun_iperf_sim.py --build \
  --duration 10 --bandwidth 250K --length 512
```

The scenario starts an ephemeral compose stack with `fps_client`, `fps_server`
and `fps_carrier`, runs UDP `iperf3` between `10.88.0.2` and `10.88.0.1` through
FPS TUN, then checks service liveness and non-zero `event=session_stats` TUN
counters.

Dante proxy overlay smoke:

```sh
FPS_DOCKER_SUDO=1 tools/docker_socks_smoke.py --build
```

This starts FPS server/client, WSS carrier, a derivative Dante proxy image and a
simple HTTP origin. The client performs a SOCKS5 TCP connect to
`10.88.0.1:1080` through TUN and receives HTTP 200 from the origin. The base FPS
image is not expected to contain Dante.

Two-client lease-routing smoke:

```sh
FPS_DOCKER_SUDO=1 tools/docker_multi_client_sim.py --build
```

This starts one FPS server and two FPS client containers with different UUIDs.
Both clients receive distinct leases, UDP probes run server-to-client and
client-to-server, a spoofed source packet is dropped by the server with
`event=ignored_spoofed_tun_source`, and valid leased traffic still works after
the spoof attempt.

Duplicate-UUID replacement smoke:

```sh
FPS_DOCKER_SUDO=1 tools/docker_duplicate_uuid_sim.py --build
```

This starts two FPS client containers with the same UUID in a staged flow. The
second active instance replaces the first for the shared lease, server logs
`event=duplicate_client_replaced`, status reports
`duplicate_client_replacements` and server-to-client traffic remains functional
only through the newer client.

Docker/TUN resilience soak smoke:

```sh
FPS_DOCKER_SUDO=1 tools/docker_resilience_soak.py --build \
  --duration 60 --clients 2 --stress-backpressure
```

This starts one server and two leased clients, runs sustained UDP plus a
concurrent TCP/HTTP probe over TUN, verifies server-to-client routing, injects a
spoofed source packet, stops and restarts one carrier client, and checks status
JSON for expected counters without exposing UUIDs or keys. The optional
`--with-socks-overlay` flag adds the derivative Dante overlay probe. The
`--stress-backpressure` phase records whether `write_queue_full` was observed;
absence of that specific counter is acceptable for routine soak if all services,
leases, routing, spoof-drop and recovery checks pass. Make it strict only for
manual tuning runs with `--require-backpressure-event` and `--write-queue-bytes`.

The same short gate is available through the quality wrapper:

```sh
FPS_DOCKER_SUDO=1 tools/run_quality_checks.sh --soak-smoke
```

For pre-public-beta soak, run the same tool longer and keep artifacts on
failure:

```sh
FPS_DOCKER_SUDO=1 tools/docker_resilience_soak.py --duration 1800 \
  --clients 2 --with-socks-overlay --stress-backpressure --keep-artifacts
```

Long soak remains opt-in because it needs Docker, `/dev/net/tun`, `NET_ADMIN`
and enough wall-clock time to observe reconnect/backpressure behavior.

### Two-Host Pre-Release Soak

The local compose soak is useful, but it does not exercise a real machine split.
For release candidates, also run a two-host Docker/TUN soak with the server stack
on a separate Linux host and clients on the local host. The server side should
publish the FPS carrier listener on an externally reachable TLS port such as
`:443`; keep carrier origin and TUN setup inside containers.

Use the repository tool instead of a one-off harness:

```sh
FPS_DOCKER_SUDO=1 tools/docker_two_host_soak.py --remote fpshop \
  --build-local --duration 300 --clients 2 --carriers-per-client 2 \
  --bandwidth 500K --length 512 --keep-artifacts
```

On weak remote hosts, keep `--build-local`: the tool builds the Alpine runtime
image locally and then loads it remotely with
`docker save | ssh ... docker load` instead of compiling there. If the image is
already present on both hosts, omit `--build-local` and pass `--image`.

The default UDP probe rate is deliberately modest (`--udp-pps 8`) because this
gate validates carrier recovery, lease routing and shaped transport liveness,
not maximum throughput. Raise it explicitly for capacity experiments. The
default loss gate allows up to `--max-loss-percent 1.0` during planned carrier
restarts; any payload mismatch, classified/envelope encode/decode error or
service exit still fails the run.

The current release-candidate shape is:

- one remote `fps_server` plus self-hosted `fps_carrier origin`;
- two local `fps_client` containers with distinct UUIDs and local
  `fps_carrier client` processes, with two carrier sessions per client by
  default;
- sustained UDP echo probes through TUN with a concurrent HTTP probe;
- spoofed-source negative probe with `ignored_spoofed_tun_source` in status;
- planned carrier stop/start, including simultaneous carrier restarts;
- final status/log checks for service liveness, non-zero FPS traffic counters,
  bad classified/envelope encode/decode events and secret-free JSON snapshots.

The tool writes a `summary.json` and redacted logs under `captures/<project>`
when `--keep-artifacts` is set or when the run fails. Keep the command shape,
image tag, duration, loss summary, spoof-drop result, carrier recovery result
and cleanup notes with the release-candidate record in `dev/WORKLOG.md`.

## CTest Labels

- `unit`: Boost.Test unit suite.
- `local`: non-sudo checks included in ordinary `ctest`.
- `integration`: Python end-to-end scenarios.
- `ops`: local operational helper checks that must not touch host networking.
- `docker`: Docker artifact/static checks.
- `log`: log/stream checks without exact timestamps.
- `wss`: WebSocket-over-TLS carrier generation and relay paths.
- `zero_rtt`: transcript-bound Zero-RTT late upgrade and classified-record path.
- `multi_carrier`: more than one authenticated carrier session.
- `shaper`: shaper-gated injected writes, adaptive TLS-record CDF training and
  snapshot bootstrap.
- `fragmentation`: TUN packet splitting/reassembly.
- `pcap`: opt-in tcpdump/libpcap wire-shape regression.
- `tun`: real Linux TUN/netns checks.
- `sudo`: tests requiring root/CAP_NET_ADMIN.

## Covered Areas

Unit tests cover:

- TLS parser/layer partial headers/bodies, coalesced records, invalid headers
  and application-data wrapping/filtering.
- Zero-RTT X25519 agreement, encrypted upgrade success, transcript-binding
  mismatch, hint precheck, unknown client fallback, malformed candidates and
  tamper rejection.
- FPS upgrade controller late upgrade, byte-for-byte fallback and fragmented
  boundary tracking.
- Classified-record and internal frame-bundle codecs, covert frames, padding,
  implicit sequence, tamper rejection and no-plaintext-metadata smoke.
- Shaper deterministic plans, CDF validation, ratio budget, proposal/commit
  scheduling, target TLS record size bounds, burst limit, backpressure
  clear/block, direction isolation, profile exhaustion, adaptive warmup,
  observed record-size/delay sampling and encrypted snapshot encode/decode.
- `TlsTcpCarrierSession` passthrough, Zero-RTT client-auth/server-accept/classify,
  client late upgrade, race-safe server-accept wait with cover-record fallback,
  fragmented server-accept wait, post-accept carrier passthrough plus classified
  record insertion, size-aware shaped classified records, shared shaper
  observation of coalesced TCP reads as complete TLS records, shaper queue
  preflight, shaper-aware opaque datagram fragmentation and unauthenticated
  enqueue rejection.
- `CovertDatagramTransport`, `TunTunnelAdapter` and `TunPacketPump` carrier
  registration/removal, generic datagram round-robin and targeted writes,
  lease-aware destination routing, strict source-IP enforcement,
  saturated-carrier fallback, same-carrier fragment policy, malformed fragment
  drops, packet boundaries, no-carrier errors, bounded TUN write queue and
  idempotent stop.
- `TunLeaseAllocator` stable persistent leases, pool exhaustion,
  list/remove/prune APIs, invalid lease-file rejection and control-frame
  encode/decode.
- Config/CLI/logging JSON parsing, required network fields, Zero-RTT key and
  allowlist validation, TUN validation, shaper config, log-level override and
  helper commands, including compact shaper CDF profile export.

Local integration tests cover:

- CLI stdout/stderr behavior, UUID generation, server keypair generation,
  generated client profiles, safe profile `--output`, `fps://` URI roundtrip,
  URI write-to-file import, unknown option rejection, `--check-config`,
  lease-management option presence, shaper profile write-to-file export and
  log-level override.
- Local `ops.status_socket` smoke: daemon publishes a UNIX status socket,
  `--status` returns JSON counters plus `sessions.last_closed` /
  `sessions.recent_closed`, root `auth` and `classified_record` counter groups,
  a compact non-secret `shaper.profile` snapshot, socket permissions are `0600`,
  and status output does not expose UUIDs or key material.
- Linux route helper dry plans for split tunnel, full tunnel policy routing,
  carrier bypass and cleanup.
- Reusable debug carrier direct roundtrip, including WSS echo and ordinary HTTPS
  `GET /` against `fps_carrier origin`.
- Docker artifact checks for Dockerfile, entrypoints, compose examples, Dante
  proxy overlay example, env contract, role-aware entrypoint aliases, shared
  `/run/fps` status volumes, configurable published carrier port, conservative
  client-host route defaults, TUN capabilities and absence of embedded secrets.
- HTTPS passthrough with no auth.
- HTTPS Zero-RTT chain, hint precheck with decoy allowlist entries, and two
  simultaneous keep-alive TLS sessions without response mixing.
- Zero-RTT adversarial local probe: direct passthrough without valid upgrade,
  unknown-client storms, transcript-prefix mismatch bursts, recovery after
  failed auth and post-auth tampered carrier records against live
  `fps_client -> fps_server -> HTTPS origin`.
- WSS passthrough, WSS Zero-RTT using reusable `fps_carrier`, and a Zero-RTT
  HTTPS browser-style request through `fps_client -> fps_server -> fps_carrier`.
- Optional pcap TLS shape check when `-DFPS_ENABLE_PCAP_TESTS=ON`.
- Local synthetic pcap shaper-profile generation check; it skips cleanly when
  libpcap is unavailable.

Opt-in real TUN integration covers:

- TUN open smoke.
- Zero-RTT loopback in isolated client/server namespaces with server-assigned
  client IPv4 lease.
- Burst, fragmentation, shaper and multi-carrier variants.

Fuzz targets:

- `fps_fuzz_tls_records`: TLS record parser/layer framing and filtering.
- `fps_fuzz_covert_codec`: decrypted covert frame decode plus valid roundtrip.
- `fps_fuzz_envelope`: internal encrypted frame-bundle decode plus valid
  roundtrip.
- `fps_fuzz_zero_rtt`: Zero-RTT candidate verify plus valid upgrade roundtrip.
- `fps_fuzz_tun_frames`: TUN lease/control payload and IPv4 packet helpers.

Coverage artifacts live in `cmake-build-coverage/coverage-summary.txt`,
`cmake-build-coverage/coverage-summary.json` and
`cmake-build-coverage/coverage-html/index.html`. The script enforces total line
and function thresholds through `FPS_COVERAGE_MIN_LINES` and
`FPS_COVERAGE_MIN_FUNCTIONS`; treat a threshold miss as a quality gate failure
unless the threshold or exclusion policy is deliberately changed. Expected low
zones: Linux TUN device open in non-sudo mode, plus operational/error branches
in `tcp_relay_app.cpp` and `tls_tcp_carrier_session.cpp`.

Fuzz artifacts live in `cmake-build-fuzz`; seed corpora live in
`tests/fuzz/corpus`. The quality script copies seeds into the build directory so
libFuzzer does not mutate tracked corpus files during smoke runs.

## Wire-Shape Checks

Manual capture utility:

```sh
tools/capture_tls_wire.sh --port 8443 --out /tmp/fps-wire.pcap -- COMMAND...
```

With `tshark` installed, inspect `/tmp/fps-wire.pcap.tls.txt`. FPS link traffic
should remain parseable as TLS records with Application Data carrying opaque
bytes. Wireshark should not lose TLS record boundaries and reinterpret the flow
as arbitrary TCP bytes.

Libpcap-based shape check:

```sh
python3 tools/is_pcap_looks_like_tls.py /tmp/fps-wire.pcap \
  --port 8443 \
  --require-bidirectional \
  --require-application-data \
  --min-records 4
```

The check validates TLS record framing and content types only. Timing and size
distribution analysis remains out of scope for the regression check.

Offline shaper profile generation from a carrier pcap:

```sh
python3 tools/pcap_to_shaper_profile.py /tmp/carrier-baseline.pcap \
  --port 443 \
  --profile-id example-origin-v1 \
  --bins 50 \
  --output profile.json
```

The profile tool uses the same libpcap/TCP/TLS parser as the TLS-shape checker.
It infers client/server direction from the TCP SYN/SYN-ACK handshake when the
capture includes it; if the capture starts later, `--port` is used as the
service-port hint. The generated JSON can be copied into `shaper` or loaded via
`shaper.profile_file`. Capture carrier-only baseline traffic, or restrict the
pcap to a known pre-upgrade time window with `--start-epoch`/`--end-epoch`; a
post-upgrade capture describes the combined visible FPS link.

For exploratory traffic-shape analysis, run the Docker/TUN capture experiment:

```sh
FPS_DOCKER_SUDO=1 tools/docker_pcap_flow_experiment.py \
  --image fps:local \
  --duration 30 \
  --bandwidth 2M \
  --iperf-bidir \
  --length 1200 \
  --carrier-bps 300000 \
  --carrier-frame-rate 30 \
  --pre-upgrade-records 60
```

The helper starts `fps_client`, `fps_server`, a debug HTTPS/WSS carrier origin,
a persistent carrier client and bidirectional UDP `iperf3` over the leased TUN
link. It captures the FPS client/server TCP link on the Docker bridge, then
writes artifacts under `captures/<project>/`:

- `fps-link.pcap`: raw capture for Wireshark or `is_pcap_looks_like_tls.py`;
- `flow-summary.json`: packet-size and inter-packet quantiles before/after the
  first observed Zero-RTT authentication;
- `flow-packets.csv`: per-packet timestamp, direction, size and phase data;
- `flow-plot.svg`: quick dependency-free scatter/heatmap view of packet sizes
  and timing.

`--carrier-bps` is bytes per second, while `iperf3 --bandwidth` is bits per
second. Use the Docker bridge capture path for TCP reassembly; capturing on
Linux `any` can duplicate or reorder Docker bridge packets enough to confuse
TLS record reconstruction.

Docker/VM endpoint captures can show GRO/GSO/TSO or hypervisor-aggregated packet
sizes that are larger than physical Ethernet frames. Use these artifacts for TLS
record syntax and coarse size/timing regressions. For physical wire-shape
claims, capture from an external tap or disable relevant offloads for the
measurement host and document that change.
Set `security.zero_rtt.client_upgrade_delay_sigma_ms=0` in measurement configs
when the exact upgrade split time matters; the beta default intentionally
randomizes the client-auth delay around `client_upgrade_delay_ms`.

For more readable research plots, use an optional local Python venv. These
packages are not FPS runtime or build dependencies:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install matplotlib pandas numpy
.venv/bin/python tools/plot_pcap_flow.py \
  --packets-csv captures/<project>/flow-packets.csv \
  --summary-json captures/<project>/flow-summary.json \
  --out-prefix captures/<project>/readable-flow
```

The plotting helper writes PNG and SVG overview/quantile figures next to the
capture artifacts. It can also run with globally installed packages, but using
`.venv` keeps exploratory dependencies out of the project environment.

See [TCP flow shape experiment](./pcap-flow-analysis.md) for a documented
example run, plots and conclusions.

## Remaining Gaps

- Transcript-bound Zero-RTT precheck is not a full CPU DoS defense.
- Pcap/TLS shape regression is opt-in because it needs `tcpdump` and sudo.
- There is no full routing/NAT-to-Internet integration scenario.
- Docker build/smoke, multi-client and proxy-overlay smokes are opt-in because
  they need a local Docker daemon, `/dev/net/tun` and `NET_ADMIN`.
- Long-running Docker/TUN soak exists as manual tooling, but it is not yet a
  scheduled privileged CI job.
- Fuzzing is bounded smoke, not a long corpus-minimization campaign.
- Shaper unit/session tests assert exact inserted classified TLS record wire
  sizes and adaptive per-TLS-record model behavior, but not yet full pcap-level
  timing/size distribution mimicry.
- Proxy overlay hardening, lease-management UX beyond list/revoke/prune,
  public upgrade guidance, release signing and realistic carrier traffic
  modeling workflows remain future work.
