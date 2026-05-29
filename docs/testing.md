# FPS Testing And Quality Workflow

The regression baseline is meant to catch changes in TLS passthrough,
Zero-RTT upgrade, classified FPS records, carrier-pool TUN scheduling,
fragmentation, shaper budgeting, CLI/config behavior and logging safety.

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

## GitHub Actions CI

The repository ships three GitHub Actions workflows:

- `CI`: runs on pull requests, pushes to `main` and manual dispatch. It covers
  `ubuntu-24.04 x gcc/clang` local builds/tests through the repository
  `Dockerfile` `ci` stage, plus Docker build/runtime smoke for Ubuntu
  GCC/clang and Alpine GCC images.
- `Quality`: runs on schedule and manual dispatch. It executes
  `tools/run_quality_checks.sh --all` inside the same `Dockerfile` `ci` stage,
  including clang-20, ASan/UBSan, Valgrind, llvm-cov and bounded libFuzzer
  smoke.
- `Publish Images`: runs on manual dispatch only. It first runs the same Docker
  runtime smoke, then can publish Ubuntu and Alpine images to GHCR when
  `publish=true`. With the default `publish=false`, it is a build-only
  deployment dry run.

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

The current beta-candidate shape is:

- one remote `fps_server` plus self-hosted `fps_carrier origin`;
- two local `fps_client` containers with distinct UUIDs and local
  `fps_carrier client` processes;
- sustained client-to-server UDP through TUN with a concurrent HTTP probe;
- server-to-client UDP probes to both leased client addresses;
- spoofed-source negative probe with `ignored_spoofed_tun_source` in status;
- carrier stop/start and post-recovery UDP probe;
- final status check for service liveness and secret-free JSON snapshots.

Do not add one-off two-host orchestration scripts to the repository unless they
become stable product tooling. Keep the command shape, image tag, duration,
throughput/loss summary, spoof-drop result, carrier recovery result and cleanup
notes with the release-candidate record.

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
- `shaper`: shaper-gated injected writes.
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
- Shaper deterministic plans, CDF validation, ratio budget, burst limit,
  backpressure clear/block, direction isolation and profile exhaustion.
- `TcpBridgeSession` passthrough, Zero-RTT strip/confirmation/classify, client late
  upgrade, race-safe confirmation wait with cover-record fallback, fragmented
  confirmation wait, post-confirmation carrier passthrough plus classified
  record insertion and unauthenticated enqueue
  rejection.
- `SessionManager` and `TunPacketPump` carrier registration/removal,
  round-robin scheduling, lease-aware destination routing, strict source-IP
  enforcement, saturated-carrier fallback, same-carrier fragment policy,
  malformed fragment drops, packet boundaries, no-carrier errors, bounded TUN
  write queue and idempotent stop.
- `TunLeaseAllocator` stable persistent leases, pool exhaustion,
  list/remove/prune APIs, invalid lease-file rejection and control-frame
  encode/decode.
- Config/CLI/logging JSON parsing, required network fields, Zero-RTT key and
  allowlist validation, TUN validation, shaper config, log-level override and
  helper commands.

Local integration tests cover:

- CLI stdout/stderr behavior, UUID generation, server keypair generation,
  generated client profiles, safe profile `--output`, `fps://` URI roundtrip,
  URI write-to-file import, unknown option rejection, `--check-config`,
  lease-management option presence and log-level override.
- Local `ops.status_socket` smoke: daemon publishes a UNIX status socket,
  `--status` returns JSON counters plus `sessions.last_closed` /
  `sessions.recent_closed`, root `auth` and `classified_record` counter groups,
  socket permissions are `0600`, and status output does not expose UUIDs or key
  material.
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
  unknown client UUID, transcript-prefix mismatch and post-auth tampered carrier
  record against live `fps_client -> fps_server -> HTTPS origin`.
- WSS passthrough, WSS Zero-RTT using reusable `fps_carrier`, and a Zero-RTT
  HTTPS browser-style request through `fps_client -> fps_server -> fps_carrier`.
- Optional pcap TLS shape check when `-DFPS_ENABLE_PCAP_TESTS=ON`.

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
in `tcp_relay_app.cpp` and `tcp_bridge_session.cpp`.

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
- There is no long-running soak/stress test for sustained TUN backpressure.
- Fuzzing is bounded smoke, not a long corpus-minimization campaign.
- Shaper does not yet assert full visible TLS record timing/size distributions.
- Production UUID/key rotation, hardened proxy overlay policy,
  lease-management UX beyond list/revoke/prune and realistic carrier traffic
  modeling workflows remain future work.
