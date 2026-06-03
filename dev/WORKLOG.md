# WORKLOG

Журнал проектных работ FPS. Новые записи добавляются сверху или в хронологическом порядке внутри текущего дня, пока проект мал.

## 2026-06-03

### Add JSON shaper profile import/export UX

Goal:

- Make shaper CDF profiles directly reusable from config/status without adding a
  separate pcap profile tool in this increment.
- Keep the public format readable JSON, not base64, and remove old object-based
  CDF config forms because there is no compatibility requirement yet.

Changes:

- Changed public shaper CDF config to compact pairs:
  `[[value, cumulative_probability], ...]`.
- Renamed public shaper inter-record delay fields to microseconds:
  `inter_record_delay_us_cdf_c2s` and `inter_record_delay_us_cdf_s2c`.
- Switched shaper scheduling delays and adaptive delay buckets to
  microseconds; jitter config remains `jitter_ms` and is converted internally.
- Changed structured shaper events to log explicit `delay_us`, avoiding silent
  sub-millisecond truncation through generic chrono-to-milliseconds JSON
  serialization.
- Added non-secret `shaper.profile` snapshots to status JSON when shaper is
  enabled. The snapshot contains compact CDF arrays, profile id, observed record
  counts and adaptive readiness metadata.
- Added `fps_client|fps_server --write-shaper-profile --config PATH --output
  PATH [--force] [--format json]`. The command exports a live status-socket
  snapshot when reachable and falls back to the static config profile otherwise.
  Output uses the existing secret-file writer (`0600`, no overwrite without
  `--force`).
- Updated unit/integration coverage for compact CDF parsing, legacy object CDF
  rejection, profile export permissions/overwrite behavior and status JSON
  shaper snapshots.
- Updated `docs/specification.md`, `docs/testing.md` and
  `docs/pcap-flow-analysis.md`.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build -R fps_unit_tests --output-on-failure`
- `ctest --test-dir build -R 'fps_status_socket|fps_cli_streams' --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`

Open:

- Offline pcap-to-profile tooling remains deferred. The current export path is
  daemon-first: run representative carriers, let adaptive CDF learn, then export
  the live profile.

### Repeat remote pcap flow analysis with offload control

Goal:

- Re-run the remote `fpshop` market-data carrier packet-size/timing capture
  after disabling endpoint offload features, so captured packet sizes are closer
  to the actual MTU-limited wire shape.
- Keep the concrete external market-data origin out of reports and docs.

Setup:

- Built local Alpine runtime image `fps:pcap-offload-68f9bed`.
- Loaded the image onto `fpshop` with `docker save ... | gzip -1 | ssh fpshop
  'gunzip | docker load'`.
- Ran `fps_server` on `fpshop` in host networking on TCP `443` and `fps_client`
  locally in host networking on `127.0.0.1:7443`.
- Used one persistent HTTPS market-data carrier connection through FPS with one
  GET about every 0.5 seconds.
- Set `client_upgrade_delay_ms=10000` and `client_upgrade_delay_sigma_ms=0` for
  a deterministic visible pre-upgrade window.
- Temporarily changed `enp1s0` on `fpshop`:
  `gro off`, `gso off`, `tso off`, `rx-gro-hw off`; restored all four to `on`
  after capture.
- The first retry waited for a non-existent `event=lease_applied` log and was
  aborted by cleanup. The second retry used `iperf3 --bidir`, which hung under
  the asymmetric shaped C2S path. The final run used bounded Python UDP
  send/receive probes instead, avoiding `iperf3` control-flow ambiguity.

Results:

- Capture artifacts are under ignored
  `captures/fps-pcap-offload-221834/`.
- TLS wire-shape checker passed:
  - total TLS records: 682;
  - client-to-server Application Data records: 177;
  - server-to-client Application Data records: 501.
- Carrier workload: 140 successful polls, about 4.17 MiB response body, no
  poller errors.
- Capture duration about 77.5 seconds; upgrade split about 10.3 seconds after
  capture start.
- Packet-size result after disabling offloads: max observed IP packet length
  was 1500 bytes before and after upgrade. This confirms that earlier 8-14 KiB
  endpoint-captured packet sizes were GRO/GSO/TSO or hypervisor aggregation
  artifacts rather than physical wire packets.
- Post-upgrade visible link:
  - 3258 packets;
  - about 0.46 Mbit/s IP throughput;
  - IP size p50/p95/max: 1500/1500/1500 bytes;
  - inter-packet p50/p95: about 0.004/27.8 ms.
- Direction-specific post-upgrade shape:
  - C2S: 600 packets, about 0.008 Mbit/s, IP p50/p95 52/308 bytes;
  - S2C: 2658 packets, about 0.45 Mbit/s, IP p50/p95 1500/1500 bytes.
- No `level=error` or `level=fatal` log entries in client/server logs.

Docs:

- Added `docs/assets/pcap-flow/market-data-remote-offload-overview.png`.
- Added `docs/assets/pcap-flow/market-data-remote-offload-quantiles.png`.
- Updated `docs/pcap-flow-analysis.md` with the offload-controlled repeat and
  interpretation.

Verification:

- `python3 tools/is_pcap_looks_like_tls.py captures/fps-pcap-offload-221834/fps-link.pcap --port 443 --require-bidirectional --require-application-data --min-records 10 --summary captures/fps-pcap-offload-221834/tls-shape.json`
- `python3 tools/analyze_pcap_tcp_flow.py captures/fps-pcap-offload-221834/fps-link.pcap --port 443 --split-time-epoch <epoch> --summary-json captures/fps-pcap-offload-221834/flow-summary.json --packets-csv captures/fps-pcap-offload-221834/flow-packets.csv --svg captures/fps-pcap-offload-221834/flow-plot.svg`
- `.venv/bin/python tools/plot_pcap_flow.py --packets-csv captures/fps-pcap-offload-221834/flow-packets.csv --summary-json captures/fps-pcap-offload-221834/flow-summary.json --out-prefix captures/fps-pcap-offload-221834/offload-aware-market-data --sample-size 0`
- Remote post-check: `ethtool -k enp1s0` showed GRO/GSO/TSO/RX-GRO-HW restored
  to `on`; `ss -ltnp` showed no leftover FPS listener on `:443`.

## 2026-06-02

### Randomize client Zero-RTT auth delay

Goal:

- Avoid a fixed, globally repeated client-auth timing offset after carrier
  eligibility while preserving deterministic test and pcap modes.

Changes:

- Added `security.zero_rtt.client_upgrade_delay_sigma_ms`.
- Default sigma is `client_upgrade_delay_ms / 3` for client configs and `0` for
  server configs.
- Each client-side TLS carrier samples one effective delay when bidirectional
  Application Data and transcript requirements first become ready:
  `clamp(client_upgrade_delay_ms + N(0, sigma_ms), 0, 2 * client_upgrade_delay_ms)`.
- `sigma_ms=0` keeps deterministic old behavior for tests and measurements.
- Updated protocol/testing/pcap docs and added unit coverage for clamp bounds,
  zero-sigma behavior and config parsing.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=tcp_relay_app,tls_tcp_carrier_session --catch_system_errors=no --log_level=test_suite`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`

### Add TCP_NODELAY runtime policy

Goal:

- Make the FPS relay TCP policy explicit before deeper shaper mimicry work.
- Keep Docker as the primary runtime while documenting its limits for
  packet-capture fidelity.

Changes:

- Added `network.tcp_no_delay`, default `true`, to the relay config.
- Applied `TCP_NODELAY` to accepted and outbound relay TCP sockets before a
  `TlsTcpCarrierSession` starts. Failure to set the option closes the candidate
  session with `set_tcp_no_delay_failed` metadata instead of silently running
  under an unknown TCP policy.
- Added a small `tcp_socket_options` helper and loopback unit coverage that
  sets and reads back the actual socket option.
- Updated check-config summaries and docs for the new option.
- Documented Docker/VM capture limitations: bridge/endpoint pcaps can show
  offload or hypervisor aggregation artifacts, so physical wire-shape claims
  require host-network/native or external-capture setups.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=tcp_relay_app --catch_system_errors=no --log_level=test_suite`
- `ctest --test-dir build -R 'fps_cli_streams' --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`

### Run 5-minute fpshop multi-client Alpine soak

Goal:

- Validate the current shaper-aware datagram fragmentation branch on the weak
  remote `fpshop` host with multiple leased clients and multiple carrier
  sessions.
- Record the future workflow decision: do not build images on weak remote soak
  hosts; build the Alpine image locally and transfer it with `docker save |
  ssh ... docker load`.

Setup:

- Built local Alpine runtime image `fps:soak-multi-19b4bf0` from current
  `develop` commit `19b4bf0`.
- Transferred the committed repository snapshot to
  `/tmp/fps-soak-multi-19b4bf0` on `fpshop` with `git archive`.
- Loaded the locally built image on `fpshop` with
  `docker save fps:soak-multi-19b4bf0 | gzip -1 | ssh fpshop 'gunzip |
  docker load'`.

Verification:

- Remote command:
  `ssh fpshop 'cd /tmp/fps-soak-multi-19b4bf0 && python3
  tools/docker_resilience_soak.py --image fps:soak-multi-19b4bf0 --duration
  300 --bandwidth 200K --length 512 --clients 2 --stress-backpressure
  --stress-bandwidth 2M --startup-timeout 90 --keep-artifacts'`.

Results:

- Main mixed client-to-server UDP: `300.05s`, `14,649` packets,
  `0%` loss, about `0.200 Mbit/s`, plus `551` HTTP probes.
- Server-to-client UDP probes to both leases: `489` packets each,
  `0%` loss.
- Backpressure stress phase: about `2.000 Mbit/s`, `2,084` packets,
  `0%` loss; Docker-level `write_queue_full` was not observed, which remains
  acceptable for this routine topology/timing-dependent stress probe.
- Spoofed-source negative path: `ignored_spoofed_tun_source=1`; valid
  post-spoof UDP stayed healthy with `0%` loss.
- Carrier stop/start recovery: one authenticated carrier closed with expected
  `peer_eof`, a replacement carrier registered, and recovered UDP stayed at
  `0%` loss.
- Final server status: two active carriers, three accepted/authenticated
  carrier sessions total, one intentional carrier removal, non-zero TUN
  packet/byte counters, zero TUN write/codec/TLS/packet-too-large failures and
  all six services running.
- Log review found no fatal/panic/assert/sanitizer failures. Expected debug
  `zero_rtt_upgrade_miss precheck_failed` entries appeared before
  authentication, expected `non_ipv4_tun_destination`/`ignored_non_ipv4`
  counters appeared from harness probes/background traffic, and carrier-origin
  `ConnectionClosedError` corresponded to the deliberate carrier restart.

Cleanup:

- Temporary remote artifacts and the temporary local/remote image tag were
  removed after log review.

### Self-review and extended validation for shaper-aware fragmentation

Goal:

- Review `5591f4b Add shaper-aware datagram fragmentation` before PR/merge
  work and run the extended validation set without soak.

Self-review:

- Queue ordering is preserved: dynamic fragmentation replaces the front shaped
  datagram with ordered fragment queue items on the same carrier.
- Shaper accounting is consistent: original datagram bytes are queued first,
  fragment header overhead is added when the datagram is expanded, and each
  fragment commit consumes `fragment_header + chunk`.
- Small datagrams still use the unfragmented `opaque_datagram` path when they
  fit the sampled TLS record; they are not penalized by fragment-header bounds.
- The lower-bound block path is intentional: if a sampled TLS record cannot fit
  even `fragment_header + 1 byte`, the datagram remains queued and no unnatural
  larger record is emitted.
- Residual limitation: fragment chunk size is fixed at the first split
  decision. Later smaller shaper samples can block the next already-numbered
  fragment until a fitting sample appears. This avoids re-fragmenting
  fragments and preserves current wire semantics.
- No secret/key/UUID/raw packet logging was introduced. Payload inspection is
  limited to unit tests.

Verification:

- `FPS_FUZZ_RUNS=64 tools/run_quality_checks.sh --all`
  - clang local suite passed;
  - ASan+UBSan local suite passed;
  - Valgrind unit pass reported 0 errors and no leaks;
  - coverage gates passed: line coverage 74.40%, function coverage 82.20%;
  - libFuzzer smoke passed for TLS records, covert codec, envelope, Zero-RTT
    and TUN frames.
- `FPS_DOCKER_SUDO=1 FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `FPS_DOCKER_SUDO=1 FPS_DOCKERFILE=Dockerfile.alpine FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:alpine tools/run_quality_checks.sh --docker`
- `cmake -S . -B cmake-build-tun -DFPS_ENABLE_TUN_TESTS=ON`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `python3 tools/docker_tun_iperf_sim.py --sudo --image fps:local --duration 10 --bandwidth 5M --length 1200`
  - 4.999 Mbit/s, 0% loss, FPS TUN stats non-zero, services alive.
- `python3 tools/docker_multi_client_sim.py --sudo --image fps:local --duration 8 --bandwidth 2M --length 900`
  - two distinct leases, bidirectional probes at about 2 Mbit/s, 0% loss,
    spoof drop event observed, services alive.
- `python3 tools/docker_duplicate_uuid_sim.py --sudo --image fps:local`
  - duplicate replacement counter observed; old client blocked, new client OK.
- `python3 tools/docker_socks_smoke.py --sudo --build --image fps:local --proxy-image fps-dante-proxy:local`
  - SOCKS HTTP probe OK, services alive.
- `python3 tools/docker_tun_iperf_sim.py --sudo --image fps:alpine --duration 8 --bandwidth 3M --length 1000`
  - 3.001 Mbit/s, 0% loss, FPS TUN stats non-zero, services alive.

## 2026-06-01

### Add shaper-aware opaque datagram fragmentation

Goal:

- Use FPS datagram fragmentation as a shaper degree of freedom instead of
  blocking every datagram larger than the currently sampled TLS record.
- Correct the pcap-flow report caveat around large endpoint-captured packet
  sizes from offload/aggregation.

Changes:

- Added a reusable `fps/net/datagram_fragment.hpp` helper for fragment payload
  construction and fragment-count calculation.
- Reused that helper in `CovertDatagramTransport`.
- Updated the authenticated shaped send path in `TlsTcpCarrierSession`:
  - shaped non-control frames are queued as individual scheduling units;
  - if a queued `opaque_datagram` does not fit the sampled TLS record, FPS
    expands it into ordered `opaque_datagram_fragment` items on the same
    carrier when the sampled record can fit a fragment header plus at least one
    data byte;
  - if even the smallest fragment cannot fit, the datagram remains queued and
    the shaper emits a blocked event for that scheduling attempt;
  - fragment header overhead is added to shaper budget accounting before the
    first fragment is committed;
  - shaped queue byte diagnostics now use pre-accounted classified-record
    sizes instead of reporting zero for not-yet-encoded records.
- Added unit coverage for:
  - large authenticated datagram split into multiple shaped TLS records and
    byte-for-byte reassembly;
  - tiny datagram staying unfragmented when it fits but a fragment header would
    not fit;
  - blocked lower-bound case where sampled TLS record is too small for a
    fragment header.
- Updated `docs/specification.md` and `docs/pcap-flow-analysis.md`.

Decisions:

- The wire format is unchanged: the existing encrypted
  `opaque_datagram_fragment` frame is reused.
- Fragment count is fixed at the first split decision. Later shaper samples can
  still block individual fragment records if their target size/padding window
  cannot fit the next fragment.
- The pcap report treats 8-14 KiB endpoint-captured packet sizes as
  GRO/GSO/TSO/hypervisor aggregation artifacts, not physical Ethernet/IP
  fragmentation evidence.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build -R 'fps_unit_tests' --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `git diff --check`

### Run remote market-data carrier pcap experiment

Goal:

- Capture and analyze the visible `fps_client <-> fps_server` TCP flow on a
  real remote host, avoiding local Docker loopback and kernel bypass effects.
- Use a dense automated HTTPS market-data carrier with a visible
  `client_upgrade_delay_ms=10000` pre-upgrade window, then send bounded TUN/FPS
  datagrams after upgrade.

Changes:

- Added two public report plots under `docs/assets/pcap-flow/`:
  `market-data-remote-overview.png` and
  `market-data-remote-quantiles.png`.
- Updated `docs/pcap-flow-analysis.md` with an anonymized remote-host
  market-data experiment section, measured packet/timing quantiles and
  interpretation.

Decisions:

- Do not document the concrete external market-data service name; the relevant
  shape is periodic HTTPS GET requests with roughly 32 KiB responses.
- Treat this as a pcap/traffic-shape experiment, not a TUN application
  throughput benchmark. Server-to-client UDP delivery was verified at the
  application socket. Client-to-server frames were verified through FPS daemon
  counters because the ad-hoc UDP socket bound to the server TUN address did
  not receive locally delivered packets in this setup.
- The experiment shows the important asymmetry of GET-response carriers:
  server-to-client covert capacity is much larger than client-to-server
  capacity. The adaptive shaper correctly blocked oversized client-to-server
  datagrams instead of forcing unnatural large request-direction records.

Verification:

- Remote build on `fpshop`:
  `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
  and `cmake --build build -j 2 --target fps_client fps_server`.
- Remote tcpdump:
  `tcpdump --immediate-mode -i <remote-iface> -s 0 -U -w fps-link.pcap 'host <client-public-ip> and tcp port 443'`.
- Carrier workload: 150 persistent-connection HTTPS GET polls at about
  0.5 second interval, about 4.81 MiB response bytes total.
- `python3 tools/is_pcap_looks_like_tls.py captures/.../fps-link.pcap --port 443 --require-bidirectional --require-application-data --min-records 10 --summary captures/.../tls-shape.json`
- `python3 tools/analyze_pcap_tcp_flow.py captures/.../fps-link.pcap --port 443 --split-time-epoch <epoch> --summary-json captures/.../flow-summary.json --packets-csv captures/.../flow-packets.csv --svg captures/.../flow-plot.svg`
- `.venv/bin/python tools/plot_pcap_flow.py --packets-csv captures/.../flow-packets.csv --summary-json captures/.../flow-summary.json --out-prefix captures/.../market-data-flow --sample-size 0`
- `git diff --check`

Results:

- Capture duration: about 75.1 s, upgrade split about 10.5 s after capture
  start, 4576 packets captured, 0 tcpdump kernel drops.
- TLS wire-shape checker: 1033 TLS records total, all observed FPS-link payload
  remained parseable as TLS records.
- Post-upgrade visible link: about 1.02 Mbit/s IP throughput, IP size p50
  261 B, IP size p95 5844 B, inter-packet p50 0.76 ms, p95 82.7 ms.
- Directional post-upgrade shape is strongly asymmetric:
  client-to-server about 0.021 Mbit/s, server-to-client about 1.00 Mbit/s.
- FPS daemon counters in the refined run: client-to-server accepted 103
  datagram frames / 6144 bytes; server-to-client delivered 240 datagram frames /
  294720 bytes.

### Add adaptive TLS-record shaper baseline

Goal:

- Move the shaper from static-only CDF scheduling toward robust traffic-shape
  mimicry by learning from real carrier TLS records.
- Keep the implementation simple: independent adaptive CDFs for record size and
  inter-record delay, shared across local sessions, with pcap/correlation tests
  deferred until the baseline is stable.

Changes:

- Added adaptive fields to `ShaperProfile`: enable flag, minimum warmup record
  count, minimum observed warmup window, decay and snapshot interval.
- Taught `Shaper` to observe parsed carrier TLS records, maintain decayed
  record-size and delay buckets, switch from static CDF to adaptive CDF after
  warmup and expose metadata snapshots.
- Added encrypted shaper snapshot control payloads. The server sends snapshots
  after authentication and periodically; clients apply matching-profile
  snapshots as advisory bootstrap data.
- Changed relay sessions to share one process-level `Shaper`, so all open
  carriers train one adaptive model instead of isolated per-session models.
- Added client-side Zero-RTT upgrade delay config. Client configs default to
  `2000 ms`, server configs to `0 ms`, giving initial carrier traffic time to
  warm the adaptive model before FPS records are inserted.
- Ensured shaper observations happen at TLS-record granularity, not TCP read
  chunk granularity, and excluded inserted classified FPS records from training.
- Updated spec/testing/pcap/roadmap/review docs for the new adaptive baseline.

Decisions:

- Independent size and delay CDFs are the first robust baseline. A joint
  size/time model such as a copula remains conditional on pcap evidence.
- Snapshot control frames contain only model metadata and profile id, never
  UUIDs, keys, nonces, raw TLS payloads or TUN/IP payload bytes.
- The client upgrade delay is checked on later carrier TLS records; it is not a
  timer that injects auth into an idle carrier.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=shaper,tls_tcp_carrier_session,tcp_relay_app,tun_lease --catch_system_errors=no --log_level=test_suite`
- `ctest --test-dir build -R 'fps_https_zero_rtt_(chain|hint_precheck|large_response|multi_session|adversarial)' --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -R 'fps_tun_zero_rtt_shaper|fps_tun_zero_rtt_loopback' --output-on-failure`
- `git diff --check`

Fixes during verification:

- Initial full CTest failed short HTTPS Zero-RTT integrations because the new
  product default client delay (`2000 ms`) intentionally waits for carrier
  warmup, while those tests close quickly. Test fixtures now explicitly set
  `client_upgrade_delay_ms=0`; product defaults remain unchanged.
- Self-review also caught a silent snapshot `profile_id` truncation risk.
  `Shaper` now rejects profile ids longer than the one-byte snapshot field.

### Add size-aware classified-record shaping

Goal:

- Start the traffic-shaping implementation by making inserted classified FPS
  records use planner-selected full TLS record wire sizes.
- Keep ordinary carrier TLS records byte-for-byte and leave adaptive
  pcap-level mimicry for a later increment.

Changes:

- Added target-size options to classified-record encoding. The encoder now pads
  encrypted classified records to an exact outer TLS Application Data record
  size when requested.
- Added precise rejection for too-small target records and preserved send
  sequence on target-size/padding validation failures.
- Split shaper scheduling into proposal and commit steps so blocked target
  sizes, padding limits and write-queue pressure do not consume queued covert
  bytes, cover budget or burst allowance.
- Wired shaped classified writes in `TlsTcpCarrierSession` to use
  `plan.tls_record_size` as the real emitted TLS record size and to report
  actual encoded size in shaper events.
- Updated the TUN shaper integration fixture so shaped profiles have enough
  configured classified-record padding capacity.
- Updated shaper/spec/testing documentation to define record-size CDF buckets as
  full TLS record wire sizes.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=fps_classified_record,shaper,tls_tcp_carrier_session --catch_system_errors=no --log_level=test_suite`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -R fps_tun_zero_rtt_shaper --output-on-failure`
- `git diff --check`

Open follow-up:

- Add pcap-level distribution checks after the shaper can schedule against
  learned or captured carrier profiles rather than static CDFs only.

## 2026-05-31

### Expand adversarial Zero-RTT regression coverage

Goal:

- Strengthen the local adversarial signal around transcript-bound Zero-RTT and
  classified-record carrier behavior before adding more product features.

Changes:

- Added Zero-RTT engine unit coverage for repeated malformed candidates,
  wrong transcript bindings, unknown clients and recovery to a valid candidate.
- Added upgrade-controller coverage for repeated non-upgrade Application Data
  passthrough and client-side fallback while waiting for a server accept record.
- Expanded the live HTTPS adversarial integration test with direct passthrough
  candidate pressure, unknown-client storms, recovery with a valid client after
  failed auth, transcript-prefix mismatch bursts and post-auth carrier tamper.
- Tightened status assertions so adversarial scenarios continue checking
  metadata-only status output without UUIDs, keys, ClientID, session keys or raw
  payload markers.
- Updated beta/testing/review docs to mark storm-style adversarial regression
  coverage as implemented while keeping CPU DoS as a protocol-review risk.

Verification:

- `python3 -m py_compile tests/integration/https_zero_rtt_adversarial.py`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=zero_rtt_upgrade,fps_upgrade_controller --catch_system_errors=no --log_level=test_suite`
- `ctest --test-dir build -R fps_https_zero_rtt_adversarial --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `FPS_JOBS=2 FPS_FUZZ_RUNS=64 tools/run_quality_checks.sh --all`
  - clang local build/tests passed;
  - ASan+UBSan local tests passed;
  - Valgrind unit pass: 0 errors, no leaks;
  - coverage: `72.57%` total lines, `80.59%` total functions;
  - libFuzzer smoke passed for TLS records, covert codec, envelope, Zero-RTT
    and TUN frames.
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `FPS_DOCKER_SUDO=1 FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `FPS_DOCKER_SUDO=1 FPS_DOCKERFILE=Dockerfile.alpine FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:alpine tools/run_quality_checks.sh --docker`
- `FPS_DOCKER_SUDO=1 FPS_DOCKER_IMAGE=fps:local FPS_DOCKER_SOAK_BUILD=0 FPS_DOCKER_SOAK_DURATION=60 FPS_DOCKER_SOAK_BANDWIDTH=500K FPS_DOCKER_SOAK_LENGTH=512 tools/run_quality_checks.sh --soak-smoke`
  - main mixed UDP: `60.05s`, `7,325` packets, `0%` loss, about
    `0.500 Mbit/s`, plus `113` HTTP probes;
  - server-to-client UDP to both leases: `1,221` packets each, `0%` loss;
  - spoofed source dropped once and valid post-spoof UDP remained healthy;
  - carrier restart/recovery passed with `1,221` recovered packets, `0%` loss;
  - all six services still running after soak;
  - Docker-level pressure phase sent about `5 Mbit/s`, `0%` loss, and did not
    observe `write_queue_full`, which remains a diagnostic condition rather
    than a required soak gate.
- `git diff --check`

Notes:

- A deliberately over-strong unit assertion around accepting a server accept
  after arbitrary racing server-to-client cover records was removed from this
  increment. Current hardening keeps the documented fallback behavior for
  non-accept records while avoiding a new protocol decision about delayed
  accept transcript binding.

### Rename TLS/TCP carrier session boundary

Goal:

- Clarify that TLS record slicing, transcript tracking and FPS classified-record
  injection belong to the concrete TLS-over-TCP carrier adapter, while
  `CovertDatagramTransport` remains the generic opaque datagram pool.
- Keep behavior and wire format unchanged.

Changes:

- Renamed active `TcpBridgeSession` types and files to
  `TlsTcpCarrierSession`.
- Renamed the thin generic carrier wrapper to
  `make_tls_tcp_carrier_adapter(...)` and
  `tls_tcp_carrier_adapter.*`.
- Updated tests, CMake sources and active docs to use the TLS/TCP carrier
  terminology.
- Added a specification boundary note for `fps_protocol_core`,
  `fps_carrier_core`, `TlsTcpCarrierSession`, `TunTunnelAdapter` and
  `fps_linux_runtime`.

Self review:

- Behavior and wire format are intentionally unchanged: this is a naming and
  documentation boundary cleanup.
- The main risk was stale active references to `TcpBridge*` in code, tests,
  CMake or public docs; the active-tree search is clean. Historical
  `dev/WORKLOG.md` entries are intentionally not rewritten.
- The rename makes the current adapter responsibility explicit: TLS record
  slicing and transcript tracking are part of the TLS-over-TCP carrier session,
  not the generic datagram transport contract.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `rg -n "TcpBridge|tcp_bridge" include src tests docs dev/REVIEW.md dev/ROADMAP.md CMakeLists.txt`
- `git diff --check`
- `FPS_JOBS=2 FPS_FUZZ_RUNS=64 tools/run_quality_checks.sh --all`
  - clang local build/tests passed;
  - ASan+UBSan local tests passed;
  - Valgrind unit pass: 0 errors, no leaks;
  - coverage: `72.57%` total lines, `80.59%` total functions;
  - libFuzzer smoke passed for TLS records, covert codec, envelope, Zero-RTT
    and TUN frames.
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `FPS_DOCKER_SUDO=1 FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `FPS_DOCKER_SUDO=1 FPS_DOCKERFILE=Dockerfile.alpine FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:alpine tools/run_quality_checks.sh --docker`
- `tools/docker_tun_iperf_sim.py --sudo --image fps:local --duration 10 --bandwidth 5M --length 1200`
  - `4.9995 Mbit/s`, `0%` loss.
- `tools/docker_tun_iperf_sim.py --sudo --image fps:alpine --duration 10 --bandwidth 5M --length 1200`
  - `5.0001 Mbit/s`, `0%` loss.
- `tools/docker_multi_client_sim.py --sudo --image fps:local --duration 10 --bandwidth 1M --length 1000`
  - two distinct leases, bidirectional UDP probes around `1 Mbit/s`, `0%`
    loss, spoof drop observed, all services alive.
- `tools/docker_duplicate_uuid_sim.py --sudo --image fps:local`
  - `duplicate_client_replacements=1`, old client blocked, new client OK.
- `tools/docker_socks_smoke.py --sudo --image fps:local --proxy-image fps-dante-proxy:local --build`
  - SOCKS HTTP probe OK, all six services alive.
- Remote 5-minute `fpshop` Docker/TUN resilience soak using temporary image tag
  `fps:soak-tlsrename-30b7ea1`:
  - command:
    `python3 tools/docker_resilience_soak.py --sudo --image fps:soak-tlsrename-30b7ea1 --duration 300 --bandwidth 200K --length 512 --clients 2 --stress-backpressure --stress-bandwidth 2M --startup-timeout 90`;
  - main mixed client-to-server UDP: `300.05s`, `14,649` packets, `0%` loss,
    about `0.200 Mbit/s`, plus `547` HTTP probes;
  - server-to-client UDP probes to both leases: `489` packets each, `0%` loss;
  - spoofed source dropped once and valid post-spoof UDP remained healthy;
  - carrier stop/start recovery passed with `489` recovered packets, `0%`
    loss;
  - final status: two active carriers, non-zero TUN counters, all six services
    running;
  - Docker-level pressure did not produce `write_queue_full`, which remains
    acceptable for routine soak because it is topology/timing dependent.
- Temporary remote directory and local/remote soak image tag were removed.

Commit:

- Local commit `Rename TLS TCP carrier session types`.

### Split carrier abstraction and build targets

Goal:

- Continue the FPS core/TUN adapter refactor by removing the concrete
  `TcpBridgeSession` dependency from `CovertDatagramTransport`.
- Reflect the architecture split in CMake targets without changing wire format,
  config schema or Docker/runtime behavior.

Changes:

- Introduced `CarrierId`, `CovertCarrier` and `CovertCarrierFrame` as the
  generic authenticated carrier contract for opaque datagram transport.
- Converted `CovertDatagramTransport` to use carrier ids and enqueue callbacks;
  direct transport tests now use fake carriers without TCP sockets.
- Added a thin `make_tcp_bridge_carrier(...)` adapter for `TcpBridgeSession`.
- Updated `TunTunnelAdapter` to keep lease/client-instance metadata keyed by
  carrier id while preserving the relay-facing session convenience API.
- Split CMake libraries into `fps_protocol_core`, `fps_carrier_core`,
  `fps_tun_adapter`, aggregate `fps_core` and `fps_linux_runtime`.
- Updated architecture docs/review/roadmap for the target split.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build -R fps_unit_tests --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `cmake -S . -B cmake-build-fuzz-smoke -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++-20 -DFPS_BUILD_TESTS=OFF -DFPS_BUILD_FUZZERS=ON`
- `cmake --build cmake-build-fuzz-smoke -j 2 --target fps_fuzz_tls_records fps_fuzz_tun_frames`
- `git diff --check`

### Collapse described enum boilerplate

Goal:

- Reduce duplicated enum declarations by replacing the common
  `enum class` + `BOOST_DESCRIBE_ENUM` pattern with Boost.Describe definition
  macros.

Changes:

- Replaced described enum declarations across core, logging and net headers
  with `BOOST_DEFINE_ENUM_CLASS`.
- Replaced fixed-underlying enums in `types.hpp` with
  `BOOST_DEFINE_FIXED_ENUM_CLASS`, preserving `std::uint8_t` underlying types.
- Kept `FrameType` in explicit `enum class : std::uint8_t` form with
  `BOOST_DESCRIBE_ENUM` because its wire values start at `1`; Boost's define
  macros forward enumerator initializers into `BOOST_DESCRIBE_ENUM`, which
  breaks generated descriptors for explicit-valued entries.

Verification:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `rg -n "BOOST_DESCRIBE_ENUM|\\benum class\\b|BOOST_DEFINE_FIXED_ENUM_CLASS|BOOST_DEFINE_ENUM_CLASS" include/fps`
- `git diff --check`

### Document datagram core and TUN adapter architecture

Goal:

- Make public docs and article drafts reflect the current architecture: FPS core
  is a covert best-effort datagram transport, while Linux TUN is the first and
  most important product adapter for VPN service.

Changes:

- Updated root and public docs to describe the current split:
  `CovertDatagramTransport` as reusable core and `TunTunnelAdapter` as the
  Linux leased L3 VPN adapter.
- Clarified Docker, routing, profile, proxy-overlay, beta-status, pcap-analysis
  and testing docs so TUN is not presented as the whole protocol.
- Updated both article drafts to remove stale wording that post-auth carrier
  TLS bytes are packed into FPS envelopes. Current wording says ordinary
  carrier TLS records are forwarded byte-for-byte and FPS inserts separate
  classified records containing opaque datagrams/control payloads.
- Updated article diagrams:
  - architecture diagram now shows `datagram core` plus `TUN adapter`;
  - TLS record stream diagram now shows interleaved carrier TLS records and
    classified FPS records carrying opaque datagrams/control.

Verification:

- `rg -n "real carrier TLS bytes inside envelopes|keeping the real carrier|carries that stream|TUN/control frames|TUN frames|TUN-кадры|transparent relay \\+ TUN|pre-reverse-proxy \\+ TUN|hidden L3 TUN tunnel|tun_packet|tun_packet_fragment" README.md docs articles`
- `dot -Tsvg articles/assets/fps-architecture.dot -o articles/assets/fps-architecture.svg`
- `dot -Tsvg articles/assets/fps-tls-record-stream.dot -o articles/assets/fps-tls-record-stream.svg`
- `git diff --check`

### Run 5-minute fpshop soak for datagram/TUN split PR

Goal:

- Validate the datagram/TUN split on the remote low-power `fpshop` host before
  opening the review PR.

Setup:

- Shipped the current `develop` snapshot and local Docker image
  `fps:soak-4757a1d` to `/tmp/fps-soak-4757a1d` on `fpshop`.
- Ran the existing Docker resilience harness remotely with two leased clients:
  `python3 tools/docker_resilience_soak.py --sudo --image fps:soak-4757a1d
  --duration 300 --bandwidth 500K --length 1200 --clients 2 --log-level debug`.

Results:

- Soak window: 300 seconds.
- All six services remained running after the test:
  `fps-server`, two `fps-client` instances, two carrier clients and one carrier
  origin.
- Initial authenticated carriers: 2; final active carriers: 2.
- Main client-to-server UDP flow: 0.500 Mbit/s, 15,625 packets, 0 lost.
- Server-to-client UDP probes to both clients: 0 lost.
- Carrier-loss recovery probe: 0.500 Mbit/s, 521 packets, 0 lost after carrier
  restoration.
- Spoofed-source negative path produced one `ignored_spoofed_tun_source` event;
  post-spoof valid UDP stayed healthy with 0 lost packets.
- TUN status showed non-zero packet/byte counters and zero write queue, codec,
  TLS record, packet-too-large and write failures.
- Expected noisy counters only: a few `non_ipv4_tun_destination` and
  `ignored_non_ipv4_tun_packet` events from harness probes/background traffic.

Conclusion:

- No soak blocker found for opening the PR. Remote validation covered leased
  multi-client TUN routing, generic datagram traffic, carrier loss/recovery and
  strict source-IP drop behavior on a separate host.

Verification:

- `ssh fpshop "cd /tmp/fps-soak-4757a1d && FPS_DOCKER_SUDO=1 python3
  tools/docker_resilience_soak.py --sudo --image fps:soak-4757a1d --duration
  300 --bandwidth 500K --length 1200 --clients 2 --log-level debug"`

### Review datagram/TUN split and run full non-soak validation

Goal:

- Self-review whether the latest architecture refactor actually separates the
  reusable FPS covert datagram core from the Linux TUN tunnel adapter.
- Run the full validation set available locally, excluding long/remote soak.

Review:

- `CovertDatagramTransport` is now the generic carrier-pool/datagram layer:
  it owns authenticated carrier registration, round-robin and targeted writes,
  opaque datagram fragmentation/reassembly, queue preflight and generic
  datagram delivery with optional source carrier metadata.
- `TunTunnelAdapter` is the Linux VPN-specific adapter over that transport:
  it owns IPv4 lease routing, strict server-side source-IP checks, duplicate
  client instance replacement and TUN-specific error/event mapping.
- `TunPacketPump` depends on `TunTunnelAdapter`, not on the generic datagram
  transport directly, which keeps Linux fd/TUN behavior outside the reusable
  transport contract.
- Relay/status/log counters use generic `datagram_*` names where the transport
  is being counted and TUN-specific `tun_tunnel_*` names where adapter policy is
  being counted.

Risks:

- The relay runtime still composes the TUN adapter directly because TUN is the
  only product adapter today. A future non-TUN consumer will still need a small
  app-level adapter around `CovertDatagramTransport`.
- `TunTunnelAdapter` intentionally mirrors carrier metadata beside the generic
  transport to keep lease policy out of the core. Tests cover replacement,
  targeted routing and source enforcement; future refactors should preserve
  that invariant.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `tools/run_quality_checks.sh --all`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `FPS_DOCKER_SUDO=1 FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `FPS_DOCKER_SUDO=1 tools/docker_tun_iperf_sim.py --image fps:local --duration 10 --bandwidth 5M --length 1200`
- `FPS_DOCKER_SUDO=1 tools/docker_multi_client_sim.py --image fps:local`
- `FPS_DOCKER_SUDO=1 tools/docker_duplicate_uuid_sim.py --image fps:local`
- `FPS_DOCKER_SUDO=1 tools/docker_socks_smoke.py --image fps:local`
- `FPS_DOCKER_SUDO=1 FPS_DOCKERFILE=Dockerfile.alpine FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:alpine tools/run_quality_checks.sh --docker`
- `FPS_DOCKER_SUDO=1 tools/docker_tun_iperf_sim.py --image fps:alpine --duration 10 --bandwidth 5M --length 1200`
- `git diff --check`

### Split covert datagram core from TUN adapter

Goal:

- Refactor the carrier core so FPS can carry generic best-effort opaque
  datagrams, with the current Linux VPN behavior implemented as a TUN adapter
  rather than being hard-wired into the frame protocol.

Changes:

- Replaced wire frame names `tun_packet`/`tun_packet_fragment` with
  `opaque_datagram`/`opaque_datagram_fragment`, preserving numeric frame values.
- Added `fps::net::CovertDatagramTransport` for authenticated carrier
  registration, round-robin/targeted writes, bounded fragmentation/reassembly
  and generic datagram delivery with source carrier metadata.
- Added `fps::net::TunTunnelAdapter` on top of the generic transport for Linux
  TUN-specific IPv4 lease routing, strict source-IP enforcement and duplicate
  client instance replacement policy.
- Updated `TunPacketPump`, relay runtime status/log counters and Docker helper
  scripts to use the TUN adapter and generic `datagram_*` frame stats.
- Added focused unit coverage for generic datagram scheduling, targeted carrier
  writes, fragmentation/reassembly, queue preflight and non-datagram frame
  rejection.
- Updated `docs/specification.md`, `docs/testing.md`, `dev/ROADMAP.md`,
  `dev/REVIEW.md` and `dev/PROTOCOL_REVIEW_BRIEF.md` to record the split.

Verification:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`

Commit:

- `df97547` (`Split covert datagram core from TUN adapter`)

### Add article UPDATE sections for classified records and pcap experiment

Goal:

- Update both article drafts with short tail sections about the current wire
  model and the packet-capture flow experiment.

Changes:

- Added UPDATE sections to the English HackerNoon draft:
  - FPS classified payloads are now inserted as separate TLS Application Data
    records, which gives the future shaper explicit scheduling units;
  - the pcap experiment confirms TLS record syntax but shows a visible
    post-upgrade traffic-shape change.
- Added the same update to the Russian Habr draft and extended its table of
  contents.
- Linked both drafts to `docs/pcap-flow-analysis.md` and the two generated
  pcap-flow plots.

Verification:

- `git diff --check`
- Manual diff review for both article drafts.

## 2026-05-29

### Run extended fpshop 10-minute Docker soak

Goal:

- Validate the current Docker/TUN runtime on the remote `fpshop` host with a
  longer, less friendly scenario than the regular smoke tests.
- Exercise more than two carrier sessions, carrier stop/start churn, two leased
  clients, bidirectional UDP, short-lived TCP application sessions, spoofed
  source-IP drops and recovery after a full temporary carrier outage for one
  client.

Setup:

- Built and transferred `fps:soak-bb88b2e-10m` from the current `develop`
  commit.
- Ran a transient remote-only harness based on existing Docker helper modules;
  no repository script was added.
- Topology: one `fps_server`, two `fps_client` containers, one
  `fps_carrier` origin and four `fps_carrier` client processes.

Results:

- Main soak window: 600 seconds; total elapsed with setup/result collection:
  634 seconds.
- All eight compose services remained running until teardown.
- Carrier churn: 26 stop/start actions; final server state had four active
  carriers.
- Zero-RTT/auth: 34 authenticated carrier sessions, no decrypt failures, no
  unknown clients and no server-accept failures.
- UDP over TUN:
  - client A to server: 0.220 Mbit/s, 32,227 packets, 0 lost;
  - client B to server: 0.180 Mbit/s, 26,368 packets, 722 lost (`2.74%`),
    expected around the forced full carrier outage;
  - server to client A: 0.120 Mbit/s, 17,579 packets, 6 lost (`0.034%`);
  - server to client B: 0.120 Mbit/s, 17,579 packets, 483 lost (`2.75%`),
    expected around the forced full carrier outage;
  - 20 second burst: 1.999 Mbit/s, 4,167 packets, 0 lost.
- Short-lived TCP echo sessions over TUN:
  - client A to server: 1,542 successes, 0 errors;
  - client B to server: 1,197 successes, 5 timeouts during outage/churn;
  - server to client A: 1,117 successes, 0 errors;
  - server to client B: 994 successes, 4 timeouts during outage/churn.

Log review:

- `level=error`/`fatal`: 0.
- `write_queue_full`, `codec_error`, `tls_record_error`, `decrypt_failed`,
  `server_accept_failed`, `unknown_client`: 0.
- Spoofed source-IP negative check produced one
  `ignored_spoofed_tun_source` event as expected.
- Fragment reassembly error counters stayed at zero.
- `unassigned_tun_destination` warnings were concentrated during the forced
  full carrier outage for client B; this matches the test design.
- Close reasons were limited to expected `peer_eof` and TCP read errors caused
  by deliberate carrier stops.

Decisions:

- Treat the B-side packet loss and TCP timeouts as expected consequences of the
  deliberate full carrier outage, not as daemon instability.
- Keep this as an operational soak report for now; do not add the transient
  two-machine harness to the repository unless this scenario becomes a regular
  release gate.

Verification artifacts:

- Local copy: `/tmp/fps-soak-10m-bb88b2e-artifacts`.
- Remote temporary files and images were removed after copying artifacts.

### Harden inbound TUN fragment reassembly

Goal:

- Fix the architecture-review finding that inbound TUN fragment reassembly used
  one global slot and could drop valid interleaved fragmented packets in
  multi-carrier or multi-client scenarios.
- Record the next architecture cleanup items without implementing them in this
  focused increment.

Changes:

- Replaced the single `SessionManager` fragment reassembly slot with bounded
  reassembly states keyed by source carrier session and `packet_id`.
- Added `SessionManagerConfig::max_fragment_reassembly_states` with default
  `64` and a metadata-only `ignored_reassembly_limit` event for overflow.
- Preserved existing per-packet ordered-fragment validation and server-side
  source-IP lease enforcement after reassembly.
- Added unit coverage for interleaved fragments across carriers, interleaved
  packets on the same carrier, mismatch isolation, bounded-state overflow and
  reassembled spoof-source drops.
- Updated `docs/specification.md`, `dev/REVIEW.md` and `dev/ROADMAP.md` with
  current behavior and remaining architecture-review follow-ups.

Decisions:

- Keep the TUN fragment wire format unchanged.
- Support interleaving across packets and carriers, but still require ordered
  fragments within each packet.
- Keep the cap internal to `SessionManagerConfig`; no JSON config or CLI surface
  is needed for this beta hardening.

Self review:

- No wire-format, config, CLI or Docker contract changes were introduced.
- Reassembly state is bounded and source-scoped, so malformed or mismatched
  fragments reset only the affected `(carrier, packet_id)` packet.
- Carrier removal, duplicate-client replacement and explicit carrier clear now
  clean associated incomplete reassembly state.
- Residual risk: incomplete fragment states have no TTL/LRU while a carrier
  stays alive. The state cap bounds memory and emits `ignored_reassembly_limit`;
  add time-based eviction only if field evidence shows this cap is noisy.

Verification:

- `cmake --build build -j 2 --target fps_unit_tests`
- `./build/fps_unit_tests --run_test=session_manager,enum_helpers --log_level=test_suite`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`
- PR-readiness sweep:
  - `FPS_JOBS=2 FPS_FUZZ_RUNS=64 tools/run_quality_checks.sh --all`
    - clang local build/tests passed;
    - ASan+UBSan local tests passed;
    - Valgrind unit pass: 0 errors, no leaks;
    - coverage: `72.12%` total lines, `80.39%` total functions;
    - libFuzzer smoke passed for TLS records, covert codec, envelope,
      Zero-RTT and TUN frames.
  - `cmake --build build -j 2`
  - `ctest --test-dir build --output-on-failure`
  - `ctest --test-dir build -L local --output-on-failure`
  - `cmake --build cmake-build-tun -j 2`
  - `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
  - `FPS_DOCKER_SUDO=1 FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
  - `FPS_DOCKER_SUDO=1 FPS_DOCKERFILE=Dockerfile.alpine FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:alpine tools/run_quality_checks.sh --docker`
  - `tools/docker_tun_iperf_sim.py --sudo --image fps:local --duration 10 --bandwidth 5M --length 1200`
    - `4.9995 Mbit/s`, `0%` loss.
  - `tools/docker_tun_iperf_sim.py --sudo --image fps:alpine --duration 10 --bandwidth 5M --length 1200`
    - `5.0001 Mbit/s`, `0%` loss.
  - `tools/docker_multi_client_sim.py --sudo --image fps:local --duration 10 --bandwidth 1M --length 1000`
    - two distinct leases, bidirectional UDP at about `1 Mbit/s`, `0%`
      loss, spoof drop observed, all services alive.
  - `tools/docker_duplicate_uuid_sim.py --sudo --image fps:local`
    - old duplicate client blocked, new client OK.
  - `tools/docker_socks_smoke.py --sudo --image fps:local --proxy-image fps-dante-proxy:local --build`
    - SOCKS HTTP probe OK, all six services alive.
- Remote 5-minute `fpshop` Docker/TUN resilience soak using temporary image tag
  `fps:soak-frag-a66f909` loaded from the local checked tree:
  - command:
    `python3 tools/docker_resilience_soak.py --image fps:soak-frag-a66f909 --duration 300 --bandwidth 200K --length 512 --clients 2 --stress-backpressure --stress-bandwidth 2M --startup-timeout 90`;
  - main mixed client-to-server UDP: `300.05s`, `14,649` packets, `0%`
    loss, about `0.200 Mbit/s`, plus `541` HTTP probes;
  - server-to-client UDP probes to both leases: `489` packets each, `0%`
    loss;
  - spoofed source dropped once and valid post-spoof UDP remained healthy;
  - carrier stop/start recovery passed with `489` recovered packets, `0%`
    loss;
  - final status: two active carriers, non-zero TUN counters, all six services
    running;
  - Docker-level pressure did not produce `write_queue_full`, which remains
    acceptable for routine soak because it is topology/timing dependent.
- Temporary remote directory and local/remote soak image tag were removed.

Notes:

- Commit: this commit, `Harden inbound TUN fragment reassembly`.

### Remove empty native packaging directories

Goal:

- Check whether `./packaging` still has tactical or strategic value now that
  Docker is the primary deployment path.

Findings:

- `./packaging` contained only empty untracked directories:
  `systemd/`, `sysusers.d/` and `tmpfiles.d/`.
- No tracked files referenced `packaging`, systemd units, native packages,
  `.deb`/RPM packaging or a native deployment path outside the historical
  `WORKLOG`.
- Current roadmap and operator docs are Docker-first; proxy overlays live under
  `examples/docker/`, and release packaging is GHCR image publication.

Changes:

- Removed the empty local `./packaging` directory tree.

Verification:

- `git ls-files packaging`
- `rg -n "packaging|systemd|native distro|native package|\\.deb|rpm|/usr/lib/systemd|fps-client\\.service|fps-server\\.service" . --glob '!dev/WORKLOG.md' --glob '!build/**' --glob '!cmake-build-*/**'`
- `find packaging -maxdepth 4 -print`

Notes:

- No commit is needed for the directory removal itself because the directories
  were not tracked. This worklog entry records the decision.

### Refresh docs and dev artifacts as current snapshots

Goal:

- Move the current local Python-refactor commit from `main` to `develop` and
  continue work there.
- Review `dev/`, `docs/`, `examples/` and `tools/` for stale public-service
  references, old-design narrative and files that should be current runbooks or
  snapshots rather than historical reports.

Changes:

- Preserved the old local `develop` as
  `backup/develop-before-python-helper-refactor`, moved
  `Reduce Python integration helper duplication` onto `develop`, and reset
  local `main` to `origin/main`.
- Rewrote `dev/UX_FLOW_REVIEW.md` from a historical public-origin test report
  into a concise current UX snapshot with operator flow, acceptable beta state
  and remaining friction.
- Rewrote `dev/NETWORK_RECOVERY.md` as a current network/capture cleanup
  runbook instead of an incident report.
- Tightened `dev/REVIEW.md` and `dev/PROTOCOL_REVIEW_BRIEF.md` wording to
  remove stale process/history phrasing; fixed the active replay note to v5.
- Removed unnecessary future-DNS-helper discussion from public carrier/routing
  docs; current docs now state the supported hosts/router-DNS override policy
  directly.
- Reworded tool comments/messages that looked like stale design markers during
  repository scans but described current pcap plotting and Docker fallback
  behavior.
- Updated the Docker artifact static test to match the current Docker build
  fallback wording.
- Removed local ignored `__pycache__` output from `tools/` and integration
  tests.

Verification:

- `rg` scans over `dev/`, `docs/`, `examples/`, `tools/`, `README.md` and
  `AGENTS.md` for stale public-origin, old compatibility and removed-design
  markers, excluding `dev/WORKLOG.md`.
- `python3 -m py_compile tools/*.py tests/integration/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `git diff --check`

Notes:

- `dev/WORKLOG.md` intentionally remains historical and still contains old
  design references.
- Commit: this commit, `Refresh docs and dev runbooks`.

### Reduce Python helper duplication

Goal:

- Apply the same small-scope duplication cleanup to Python integration scripts
  and Docker scenarios.
- Keep behavior, test names and public workflows unchanged.

Changes:

- Added `tools/fps_docker_common.py` for shared Docker/subprocess primitives,
  compose execution, service/log waiters, server key generation, JSON writes,
  iperf UDP summary parsing and session-stat helpers.
- Migrated Docker simulation scripts to use the shared helper module instead of
  importing generic helpers from `docker_tun_iperf_sim.py` or keeping local
  copies.
- Kept scenario-specific config/compose rendering local to each script, where
  it still documents the scenario contract.
- Added `ZeroRttRelayPair` to `tests/integration/fps_https_harness.py` and
  migrated repeated HTTPS/WSS Zero-RTT relay setup/teardown code to it.
- Preserved explicit regression markers such as `event=session_stats` in the
  scenario files so static artifact tests still validate the intended checks.

Verification:

- `python3 -m py_compile tools/*.py tests/integration/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `python3 tools/docker_tun_iperf_sim.py --help >/dev/null && python3 tools/docker_multi_client_sim.py --help >/dev/null && python3 tools/docker_duplicate_uuid_sim.py --help >/dev/null && python3 tools/docker_resilience_soak.py --help >/dev/null && python3 tools/docker_socks_smoke.py --help >/dev/null && python3 tools/docker_pcap_flow_experiment.py --help >/dev/null`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `git diff --check`

Notes:

- No protocol, Docker topology, CLI, config or test behavior changes.
- Commit: this commit, `Reduce Python integration helper duplication`.

### Reduce helper duplication in core and tests

Goal:

- Reduce source/test duplication without changing protocol, runtime behavior or
  public UX.
- Keep the refactor small enough to validate with local and TUN regression
  suites.

Changes:

- Added shared protocol constants for current FPS wire version, hint size and
  default frame/envelope limits.
- Moved repeated byte-appending helpers into `fps/core/wire.hpp` and reused
  them across Zero-RTT, classified records, envelopes, covert codec and bridge
  I/O.
- Added `enum_name_at` on top of Boost.Describe and replaced manual status
  counter index-to-name switches in relay status JSON.
- Factored duplicated SessionManager carrier enqueue loops into one helper that
  preserves round-robin, leased-destination routing and write-queue fallback
  semantics.
- Added `tests/support/fps_test_helpers.hpp` for repeated unit-test fixtures:
  deterministic byte vectors, X25519 test keys, session keys, TLS record
  helpers and connected socket pairs.
- Migrated repeated unit-test helpers to the shared support header and formatted
  the touched C++ files with `clang-format-20`.
- Added focused classified-record pipeline tests for sequence accessors,
  TLS-record encode errors and pending TLS byte reporting after the first
  extended coverage run showed function coverage at `79.80%`, below the `80%`
  gate.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `FPS_FUZZ_RUNS=16 tools/run_quality_checks.sh --fuzz`
- `FPS_JOBS=2 FPS_FUZZ_RUNS=64 tools/run_quality_checks.sh --all`
  - clang local build/tests passed;
  - ASan+UBSan local tests passed;
  - Valgrind unit pass: 0 errors, no leaks;
  - coverage: `71.97%` total lines, `80.30%` total functions;
  - libFuzzer smoke passed for TLS records, covert codec, envelope, Zero-RTT
    and TUN frames.
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `FPS_DOCKER_SUDO=1 FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- Documentation consistency grep over `docs/`, `README.md` and `articles/`
  found no stale user-facing markers for old research docs, old compatibility
  language, removed public echo/hold-wss flow, Postman, or explicit GFW/TSPU
  references.

Notes:

- Docker image/simulation soak was intentionally not run for this PR; Docker
  smoke/build and compose validation passed.
- Commit: this commit, `Reduce helper duplication in core and tests`.

### Run v5 self-review and full quality suite

Goal:

- Self-review the current Zero-RTT v5 candidate.
- Run the full local/non-Docker quality suite, opt-in TUN tests, Docker runtime
  simulations and a 5-minute remote soak on `fpshop`.

Findings and fixes:

- Coverage gate initially failed narrowly after v5 (`79.90%` function coverage
  versus the `80%` threshold). The cause was a stale, unused
  `send_client_instance_metadata` relay helper left after client instance
  metadata moved into encrypted client-auth payload. Removed the dead helper.
- Fuzz build initially failed because `tests/fuzz/fuzz_zero_rtt.cpp` still used
  the old one-direction `ZeroRttChannelBinding`. Updated the harness to use
  `ZeroRttHandshakeBinding` and to exercise client-auth plus server-accept.
- Opt-in TUN tests found a real v5 regression: the client delivered encrypted
  server-accept control payload before the relay had registered the carrier, so
  leased IPv4 auto-config was dropped as an unauthenticated control frame.
  Fixed `TcpBridgeSession` to call `on_zero_rtt_authenticated` before delivering
  `server_accept_payload`, and added a unit assertion for that ordering.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `FPS_JOBS=2 FPS_FUZZ_RUNS=64 tools/run_quality_checks.sh --all`
  - clang local build/tests passed;
  - ASan+UBSan local tests passed;
  - Valgrind unit pass: 0 errors, no leaks;
  - coverage: `71.96%` total lines, `80.03%` total functions;
  - libFuzzer smoke passed for TLS records, covert codec, envelope, Zero-RTT
    and TUN frames.
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `FPS_DOCKER_SUDO=1 FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `FPS_DOCKER_SUDO=1 FPS_DOCKERFILE=Dockerfile.alpine FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:alpine tools/run_quality_checks.sh --docker`
- `tools/docker_tun_iperf_sim.py --sudo --image fps:local --duration 10 --bandwidth 5M --length 1200`
  - `4.9995 Mbit/s`, `0%` loss.
- `tools/docker_tun_iperf_sim.py --sudo --image fps:alpine --duration 10 --bandwidth 5M --length 1200`
  - `5.0003 Mbit/s`, `0%` loss.
- `tools/docker_multi_client_sim.py --sudo --image fps:local --duration 10 --bandwidth 1M --length 1000`
  - two distinct leases, bidirectional UDP probes at about `1 Mbit/s`,
    `0%` loss, spoof drop observed, all services alive.
- `tools/docker_duplicate_uuid_sim.py --sudo --image fps:local`
  - `duplicate_client_replacements=1`, old client blocked, new client OK.
- `tools/docker_socks_smoke.py --sudo --image fps:local --proxy-image fps-dante-proxy:local --build`
  - SOCKS HTTP probe OK, all six services alive.
- Remote 5-minute `fpshop` Docker/TUN resilience soak using temporary image tag
  `fps:soak-v5-76b1d43-qa` loaded from the local checked tree:
  - command:
    `python3 tools/docker_resilience_soak.py --image fps:soak-v5-76b1d43-qa --duration 300 --bandwidth 200K --length 512 --clients 2 --stress-backpressure --stress-bandwidth 2M --startup-timeout 90`;
  - main mixed client-to-server UDP: `300.01s`, `14,649` packets,
    `0%` loss, about `0.200 Mbit/s`, plus `546` HTTP probes;
  - server-to-client UDP probes to both leases: `489` packets each, `0%` loss;
  - spoofed source dropped once and valid post-spoof UDP remained healthy;
  - carrier stop/start recovery passed with `489` recovered packets, `0%`
    loss;
  - final status: two active carriers, non-zero TUN counters, all six services
    running;
  - Docker-level pressure did not produce `write_queue_full`, which remains
    acceptable for routine soak because it is topology/timing dependent.
- Temporary remote directory and local/remote soak image tag were removed.

Open notes:

- Coverage now passes with a narrow function-coverage margin. The next code
  cleanup should either add focused coverage for live relay paths or adjust the
  coverage accounting to avoid header-only Boost.Describe noise if it becomes a
  recurring false signal.

### Implement Zero-RTT v5 1-RTT finalization

Goal:

- Close the fast c2s replay surface left by the v4 classified-record baseline.
- Require a server accept leg before final classified-record keys exist.
- Ensure FPS does not send an auth response until the carrier has produced TLS
  Application Data in both directions.

Decisions:

- Keep the current class names for now (`ZeroRttUpgradeEngine`,
  `FpsUpgradeController`) to avoid a broad rename-only diff.
- Use a bidirectional handshake binding for client auth and server accept, while
  keeping per-direction transcript bindings for post-auth classified records.
- Carry the client runtime instance id in the encrypted client-auth payload and
  carry the assigned TUN lease in the encrypted server-accept payload.
- Keep timestamp/replay-cache fields out of active config/wire format; replay
  resistance now depends on the bidirectional carrier transcript plus the server
  accept leg.

Done:

- Bumped active Zero-RTT/classified-record wire version to `5`.
- Split authentication into client-auth and server-accept records with separate
  hint labels, encrypted capsules and final session-key derivation.
- Gated channel opening on observed TLS Application Data in both directions.
- Updated `TcpBridgeSession` and relay runtime integration so server-side lease
  assignment happens before accept and reaches the client inside accept payload.
- Updated unit/integration fixtures, Docker examples and public protocol docs
  for v5.
- Adjusted the adversarial replay-style test: c2s-only replay/mutation must not
  authenticate under the bidirectional gate, but it no longer necessarily
  increments a precheck-failure counter.

Verification:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`

Commit:

- This logical commit: `Implement Zero-RTT v5 1-RTT finalization`

### Make TCP bridge TLS framing session-owned

Goal:

- Fix the remaining PR #4 framing risk: TCP `read_some` boundaries must not be
  treated as TLS record boundaries before, during or after Zero-RTT upgrade.
- Add failing regressions first, then refactor the bridge path so upgrade,
  cover and classified processing all consume complete TLS records.

Finding:

- The previous coalesced-record fix handled a complete post-auth TLS record in
  the same read as the upgrade, but it still relied on separate parsers in
  upgrade/classified/cover layers. A malicious or unlucky TCP segmentation could
  split the next TLS record across the auth transition and lose parser state.

Done:

- Added bridge regressions for:
  - a post-auth client-to-server carrier TLS record split immediately after the
    upgrade record;
  - a server-to-client origin TLS record split across the client auth
    confirmation transition.
- Moved stream slicing ownership into `TcpBridgeSession`: each direction now has
  one `TlsRecordParser`, and the bridge dispatches complete `TlsRecord`s to
  cover, Zero-RTT and classified-record processors.
- Reduced `FpsUpgradeController` to record-level observation/verification and
  removed the temporary `post_auth_bytes` handoff.
- Added record-level entry points to cover/classified pipelines for bridge use
  while keeping their byte-stream helpers for unit tests and lower-level users.
- Updated unit tests to match the record-level upgrade-controller contract.

Verification:

- `cmake --build build -j 2`
- `./build/fps_unit_tests --catch_system_errors=no --run_test=fps_upgrade_controller,tcp_bridge_session`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `FPS_FUZZ_RUNS=64 tools/run_quality_checks.sh --all`
- `git diff --check`

Commit:

- This commit: `Make TCP bridge TLS framing session-owned`

### Final v4 PR self-review fix

Goal:

- Re-review PR #4 before merge and verify that the v4 classified-record
  transition handles coalesced TCP reads safely.
- Check public docs/articles for stale v3/envelope-mode wording after the v4
  protocol refactor.

Finding:

- Self-review found one blocker in the server pre-auth path: if one TCP read
  contained the successful Zero-RTT upgrade TLS record plus a later carrier TLS
  record, `FpsUpgradeController::process_inbound_tls()` authenticated on the
  upgrade and then silently consumed the post-auth record instead of handing it
  to classified-record processing.

Done:

- Added `FpsUpgradeProcessResult::post_auth_bytes`.
- Changed `FpsUpgradeController` to return TLS records observed after the
  auth record in the same parse batch for post-auth classification instead of
  updating the transcript and dropping them.
- Updated `TcpBridgeSession` server auth transition to process those
  `post_auth_bytes` through the classified-record pipeline and forward ordinary
  carrier bytes in order.
- Added controller-level and bridge-level regressions for upgrade-plus-following
  TLS record coalescing.
- Refreshed the HackerNoon/Habr article text where it still described the old
  envelope-mode runtime and visible Zero-RTT prefix as current behavior.

Verification:

- `cmake --build build -j 2`
- `./build/fps_unit_tests --catch_system_errors=no --run_test=fps_upgrade_controller,tcp_bridge_session`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `tools/run_quality_checks.sh --all`
- `git diff --check`

Commit:

- This commit: `Fix coalesced post-auth TLS handoff`

## 2026-05-28

### Implement Zero-RTT v4 classified FPS records

Goal:

- Keep explicit Zero-RTT authentication, but stop wrapping the whole post-auth
  carrier TLS byte stream into FPS envelopes.
- Make ordinary carrier TLS records continue byte-for-byte after auth and send
  covert TUN/control payloads as separately inserted, transcript-classified TLS
  Application Data records.

Done:

- Added `FpsClassifiedRecordCodec` and `FpsClassifiedRecordPipeline`.
- Bumped active Zero-RTT config/wire version to 4 and rejected stale explicit
  versions.
- Reworked `FpsUpgradeController` transcript tracking so both directions can be
  observed before and after upgrade.
- Reworked `TcpBridgeSession` authenticated I/O:
  - carrier TLS records are forwarded byte-for-byte and transcript-tracked;
  - classified FPS records are decoded/swallowed before reaching TLS endpoints;
  - TUN/control frames are encoded as inserted classified records;
  - shaper queue preflight keeps classified-frame batches atomic.
- Updated relay logs/status to expose `classified_record` counters instead of
  old post-auth envelope-mode counters.
- Updated examples, integration fixtures, specification, protocol review notes
  and beta/testing docs for the v4 classified-record baseline.

Decision:

- Keep `FpsEnvelopeContent`/frame-bundle codec as an internal encrypted payload
  representation and fuzz/unit-test target.
- Remove the old runtime envelope callbacks from `TcpBridgeSession`; external
  runtime/status terminology is now classified-record based.
- Treat the fully handshake-less classifier as future protocol work, not this
  beta increment.

Verification:

- `cmake --build build -j 2`
- `./build/fps_unit_tests --catch_system_errors=no --run_test=enum_helpers,tcp_bridge_session,tcp_relay_app,fps_classified_record,zero_rtt_upgrade,fps_upgrade_controller`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `python3 -m py_compile tests/integration/*.py tools/*.py && bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh && ctest --test-dir build --output-on-failure`
- `cmake --build build -j 2 && ctest --test-dir build -L local --output-on-failure && git diff --check`
- `python3 -m py_compile tests/integration/*.py tools/*.py && bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh && python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `ctest --test-dir build --output-on-failure`
- PR-readiness quality sweep on 2026-05-29:
  - `tools/run_quality_checks.sh --all`
  - `FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
  - `tools/run_quality_checks.sh --all` coverage gate:
    72.22% total line coverage, 80.37% total function coverage.
- Short `fpshop` Docker/TUN soak on 2026-05-29:
  - image tag: `fps:soak-v4-0e732ca`;
  - command shape:
    `python3 tools/docker_resilience_soak.py --image fps:soak-v4-0e732ca --duration 300 --bandwidth 200K --length 512 --clients 2 --stress-backpressure --stress-bandwidth 2M --startup-timeout 90`;
  - main mixed client-to-server UDP: 300.05 seconds, 14,649 packets,
    0 lost, about 0.20 Mbit/s, plus 547 HTTP probe requests;
  - server-to-client UDP probes to both leases: 489 packets each, 0 lost;
  - spoofed source dropped once and valid post-spoof UDP remained healthy;
  - carrier stop/start recovery passed with 489 recovered UDP packets, 0 lost;
  - final status: two active carriers, non-zero TUN counters, all six services
    running; routine Docker-level backpressure did not trigger `write_queue_full`
    on this host, which is acceptable under the documented soak policy.
  - temporary `/tmp/fps-soak-v4-0e732ca` runtime and the remote/local
    `fps:soak-v4-0e732ca` tags were removed after the run.

### Add FPS TCP-flow pcap shape experiment

Goal:

- Capture and analyze how the visible `fps_client <-> fps_server` TCP flow
  changes before and after Zero-RTT upgrade under a continuous WSS carrier and
  comparable TUN traffic.

Done:

- Added `tools/analyze_pcap_tcp_flow.py`, a libpcap-based analyzer that filters
  a TCP flow by port/endpoints and writes:
  - packet/inter-packet quantiles before and after a supplied split time;
  - per-direction summaries;
  - per-packet CSV;
  - dependency-free SVG scatter/heatmap plots.
- Added `tools/docker_pcap_flow_experiment.py`, a Docker/TUN experiment harness
  that starts FPS client/server, debug WSS carrier origin/client, captures the
  FPS TCP link on the Docker bridge, runs UDP `iperf3` through TUN and invokes
  the analyzer.
- The harness defaults to resolving the concrete Docker bridge interface. A
  first capture on Linux `any` showed why this matters: duplicated/reordered
  bridge packets can make TCP/TLS reassembly report sequence gaps.
- Documented the workflow in `docs/testing.md`.

Experiment results:

- One-way UDP over FPS at `iperf3 -b 2M`: 30 seconds, 6,250 packets, 0 lost.
- Bidirectional UDP over FPS at `iperf3 --bidir -b 2M`: 30 seconds, both
  directions about 2.0 Mbit/s, 0 lost.
- TLS-shape validation on the bidirectional capture passed: 14,697 TLS records
  across both directions, all FPS-link bytes parse as TLS records.
- Bidirectional capture artifacts:
  `captures/fps-pcap-flow-bidir-221120/`.

Observation:

- Before upgrade, the visible carrier flow was dominated by regular large WSS
  echo records around the 30 fps carrier cadence.
- After upgrade plus bidirectional 2 Mbit/s TUN traffic, the flow shows a clear
  new concentration of near-1321-byte IP packets and much shorter inter-packet
  intervals: p50 inter-packet time changed from about 0.19 ms overall before
  upgrade to about 0.82 ms after upgrade, while p95 changed from about 33 ms to
  about 4.3 ms. Per direction after upgrade, visible throughput was about
  4.4 Mbit/s each way.
- This is useful evidence that the next traffic-analysis work should focus on
  envelope scheduling/shaping against the carrier's visible packet-size and
  timing distribution, not only on TLS-record syntactic validity.

Verification:

- `python3 -m py_compile tools/analyze_pcap_tcp_flow.py tools/docker_pcap_flow_experiment.py`
- `FPS_DOCKER_SUDO=1 tools/docker_pcap_flow_experiment.py --image fps:local --duration 30 --bandwidth 2M --iperf-bidir --length 1200 --carrier-bps 300000 --carrier-frame-rate 30 --pre-upgrade-records 60 --project fps-pcap-flow-bidir-221120`
- `python3 tools/is_pcap_looks_like_tls.py captures/fps-pcap-flow-bidir-221120/fps-link.pcap --port 8443 --require-bidirectional --require-application-data --min-records 10 --summary captures/fps-pcap-flow-bidir-221120/tls-shape.json`
- `git diff --check`

Follow-up:

- Initialized a local `.venv` for exploratory plotting only:
  `matplotlib`, `pandas` and `numpy`.
- Added `.venv/` to `.gitignore`.
- Added `tools/plot_pcap_flow.py`, an optional plotting helper that reads
  `flow-packets.csv` and `flow-summary.json` and writes readable PNG/SVG
  overview and quantile charts.
- Generated readable plots for the bidirectional capture:
  - `captures/fps-pcap-flow-bidir-221120/readable-flow.overview.png`
  - `captures/fps-pcap-flow-bidir-221120/readable-flow.quantiles.png`

Additional verification:

- `.venv/bin/python tools/plot_pcap_flow.py --packets-csv captures/fps-pcap-flow-bidir-221120/flow-packets.csv --summary-json captures/fps-pcap-flow-bidir-221120/flow-summary.json --out-prefix captures/fps-pcap-flow-bidir-221120/readable-flow`

Documentation:

- Added `docs/pcap-flow-analysis.md` with the experiment scenario, commands,
  committed overview/quantile plots and conclusions.
- Linked the page from `docs/index.md` and `docs/testing.md`.

Documentation verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build -L local --output-on-failure`
- `git diff --check`

### Run PR-readiness checks after Zero-RTT v3

Goal:

- Recheck documents/source/tests after transcript-bound Zero-RTT v3 and run a
  short remote soak before preparing a PR.

Done:

- Rechecked active public docs and source tree for stale v2/ClientID/replay
  contract wording. Remaining matches are historical worklog entries, negative
  removed-field tests, or explicit secret-leak guards.
- Fixed a race in `https_zero_rtt_adversarial.py`: the tampered-envelope probe
  now first establishes an authenticated keep-alive carrier and only then arms
  the proxy to mutate the next server-to-client FPS envelope. This makes the
  ASan/UBSan run deterministic instead of occasionally tampering pre-upgrade TLS
  Application Data.
- Added coverage for the `Repeater::maybe_do(interval, fn)` convenience
  overload so the coverage gate stays above the function threshold.
- Ran a 5-minute remote Docker/TUN resilience soak on `fpshop` using image
  `fps:soak-pr-v3`; temporary remote files and image tag were removed
  afterwards.

Remote soak result:

- Main mixed client-to-server UDP: 300.05 seconds, 14,649 packets, 0 lost,
  0.0% loss, about 0.20 Mbit/s.
- Concurrent HTTP probe: 543 successful requests.
- Server-to-client UDP probes to both leased clients: 489 packets each, 0 lost,
  0.0% loss.
- Spoofed source was dropped once and valid post-spoof UDP remained healthy.
- Carrier stop/start recovery passed; recovered UDP had 489 packets, 0 lost.
- Final status showed two active carriers, non-zero TUN traffic counters and all
  six services still running.

Verification:

- `tools/run_quality_checks.sh --all`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- Remote:
  `ssh fpshop 'cd /tmp/fps-soak-pr-v3 && python3 tools/docker_resilience_soak.py --image fps:soak-pr-v3 --duration 300 --bandwidth 200K --length 512 --clients 2 --stress-backpressure --stress-bandwidth 2M --startup-timeout 90'`

Commit:

- Local commit: `Stabilize PR readiness checks`.

### Implement transcript-bound Zero-RTT v3

Goal:

- Replace previous-record-only Zero-RTT binding with full carrier transcript
  binding and remove active timestamp/replay-cache mechanics from the
  pre-production protocol.

Done:

- Changed Zero-RTT candidate wire shape to
  `server_hint[8] | client_hint[8] | encrypted_capsule | tag`.
- Moved the client ephemeral public key into the encrypted capsule, removing
  the visible public-key-shaped prefix.
- Added per-direction transcript tracking in `FpsUpgradeController`, binding
  candidates to transcript hash, transcript byte count, record index, direction
  and profile id.
- Removed active timestamp/replay nonce/replay cache fields from core structs,
  relay config parsing, generated profiles, status counters and tests.
- Fixed Zero-RTT config to accept only v3 when `security.zero_rtt.version` is
  present, and renamed active examples/fixtures away from stale `v2` profile
  ids.
- Updated local adversarial integration coverage from replay-cache observation
  to transcript-prefix mismatch observation.
- Updated public and developer docs for the v3 no-timestamp/no-cache replay
  model and remaining protocol-review questions.

Decision:

- Keep explicit upgrade/envelope mode as the beta baseline. Handshake-less
  classification remains a separate protocol-review item.
- Do not keep compatibility for removed `timestamp_window_sec`,
  `replay_cache_size` or `trial_decrypt_limit` config fields; they now fail
  config validation as invalid Zero-RTT v3 fields.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `git diff --check`

Commit:

- Local commit: `Implement transcript-bound Zero-RTT v3` (see `git log --oneline -1`).

### Document transcript-bound Zero-RTT direction

Goal:

- Analyze the proposed protocol direction: replace previous-record-only
  Zero-RTT binding with full carrier transcript binding, and evaluate whether it
  should become the next technical increment.

Done:

- Updated `docs/specification.md` with a future transcript-bound wire revision:
  per-direction incremental transcript hash, public domain-separated seed
  material, transcript byte count and record index binding.
- Documented that transcript binding makes organic cross-session replay
  impractical under an honest/non-malicious origin, but does not remove the need
  for timestamp and replay-cache checks.
- Added a candidate server/client hint classifier design and explicitly marked
  it as a passive/random-traffic filter rather than a complete active DoS
  defense.
- Captured handshake-less envelope classification as a larger v3 research path
  with strict false-positive/false-negative and transcript-state requirements.
- Updated the protocol review brief, beta status, roadmap and self-review so an
  external reviewer can evaluate this direction before code changes.

Decision:

- Treat transcript-bound Zero-RTT as the right next protocol simplification
  direction, but do not implement it until the exact transcript state, hint
  derivation and failure behavior are specified and tested.
- Keep explicit upgrade/envelope mode as the safer beta baseline until
  handshake-less classification is reviewed separately.

Verification:

- `git diff --check`

## 2026-05-22

### Rename publish workflow for public repository

Goal:

- Remove private-repository wording from the active GHCR image publication
  workflow and documentation before publishing the first public package.

Done:

- Renamed `.github/workflows/publish-private-images.yml` to
  `.github/workflows/publish-images.yml`.
- Renamed the workflow from `Publish Private Images` to `Publish Images`.
- Updated release/testing/beta-status docs and GitHub operations notes for the
  public repository state.
- Tightened `main` branch protection to require pull requests with zero
  approvals while there are no write-access collaborators.

Planned verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`
- PR CI before merge
- manual `Publish Images` dispatch with `publish=true`

### Prepare article/docs PR

Goal:

- Prepare the current `develop` article/documentation branch for a normal
  GitHub PR into `main`.

Done:

- Rebasing `develop` onto `origin/main` kept the PR focused on the article,
  article assets and the small documentation updates, without replaying the
  already-merged workflow/registry history.
- Verified the branch locally before push.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check origin/main..develop`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`

## 2026-05-21

### Add Habr article TOC and references

Goal:

- Apply the user's latest Habr article edits: resolve inline comments, add a
  table of contents and convert source mentions into reference-style Markdown
  links.

Done:

- Added a table of contents with stable HTML anchors to
  `articles/free-porn-storage-fps-habr-ru.md`.
- Added reference-style links in the article body and moved URL targets to the
  bottom references section.
- Added links for HTTPS adoption, ECH, SNI, JA3/JA4, TLS 1.3, active probing,
  TLS-in-TLS, Wolf LANSEC '89, VLESS/XTLS, NaïveProxy, Cloak, Balboa, Tor
  pluggable transports and Shadowsocks.
- Replaced the remaining `{...}` Xray/VLESS/XTLS comment with a concrete
  technical comparison: XTLS/Vision optimizes a proxy-flow path near connection
  startup, while FPS late-upgrades an already real TLS carrier and carries L3
  TUN frames through a carrier pool.
- Reduced remaining high-noise mixed English terms in the edited article.

Verification:

- reference-style link definition check for
  `articles/free-porn-storage-fps-habr-ru.md`
- local Markdown asset/link check for `articles/free-porn-storage-fps-habr-ru.md`
- `rg -n '\{[^}]+\}|формИ|envelops|fingerprint-ами|fallback|TLS-like|reverse proxy|origin|carrier byte|record boundaries' articles/free-porn-storage-fps-habr-ru.md || true`
- `git diff --check`

## 2026-05-20

### Polish Russian Habr article terminology

Goal:

- Apply the user's Habr article edits by reducing unnecessary mixed-language
  phrasing and resolving inline editor comments without changing the technical
  message.

Done:

- Replaced high-noise English terms in
  `articles/free-porn-storage-fps-habr-ru.md` with common Russian equivalents
  where precision was preserved: authentication, fallback, byte stream, payload,
  record boundaries, carrier pool, proxy protocol, link obfuscation framework,
  endpoint, deployment model and related terms.
- Kept protocol names and common identifiers in English where translating them
  would make the text less clear: TLS, SNI, ALPN, TUN, UUID, WebSocket,
  Zero-RTT, AEAD, CI and similar.
- Resolved the remaining inline `{...}` comment by making the Tor pluggable
  transports comparison more concrete.
- Fixed the NaïveProxy reference URL to the canonical lowercase GitHub path.

Verification:

- `rg -n '\{[^}]+\}' articles/free-porn-storage-fps-habr-ru.md || true`
- `rg -n '\b(authentication|framework|fallback|fingerprint|handshake|payload|record|records|carrier|cover|upgrade|endpoint|origin|lease|allowlist|keypair|candidate|ephemeral|primary session|reconnect|shaping|padding|traffic analysis|byte stream|proxy protocol|link obfuscation framework|anonymity|properties|security boundary|middlebox|cipher suites|extensions|browser-like|TLS-oriented|UUID-identity|HTTPS-like)\b' articles/free-porn-storage-fps-habr-ru.md || true`
- local Markdown link check for `articles/free-porn-storage-fps-habr-ru.md`
- `git diff --check`

### Add Russian Habr article draft

Goal:

- Add a Russian-language Habr-oriented version of the FPS article under
  `articles/`, reusing the existing article diagrams and shifting the tone
  toward a more technical Habr-style essay.

Done:

- Added `articles/free-porn-storage-fps-habr-ru.md`.
- Adapted the HackerNoon draft into Russian instead of making a literal
  translation:
  - censorship/filtering escalation first;
  - TLS record-layer explanation before the FPS design;
  - FPS presented as a late-authentication TLS steganography experiment;
  - same Graphviz-rendered SVG illustrations reused from `articles/assets/`.
- Kept the article body generic and avoided explicit named filtering systems.
- Avoided references to the style-only article URL in project files.

Verification:

- local Markdown link check for `articles/free-porn-storage-fps-habr-ru.md`
- `git diff --check`

Decision:

- Keep the Russian publication draft next to the English draft in `articles/`;
  public operator docs remain under `docs/` and are not linked to this narrative
  draft yet.

### Review article and docs consistency

Goal:

- Do a shallow consistency pass across public docs and the current article
  draft before more publication-oriented writing.

Done:

- Checked public docs/articles for stale workflow names, old docs paths,
  removed carrier helpers, embedded SOCKS references and obsolete tag examples.
- Verified local Markdown links in `README.md`, `docs/*.md` and
  `articles/*.md`.
- Normalized the TLS stream diagram label from `ZRTT` to `Zero-RTT`.
- Updated `docs/beta-status.md` to reflect that `main` branch protection is
  configured in the current private repository, not merely expected.

Verification:

- `rg -n 'release-candidate\.yml|Release Candidate|four GitHub Actions workflows|v0\.1\.0-beta\.1-ubuntu|hold-wss|echo\.websocket\.org|docs/beta-readiness|docs/client-ux|research/' README.md docs articles dev AGENTS.md tests/integration/docker_artifacts.py .github || true`
- local Markdown link check for `README.md`, `docs/*.md` and `articles/*.md`
- `dot -Tsvg articles/assets/fps-tls-record-stream.dot -o articles/assets/fps-tls-record-stream.svg`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`

Decision:

- Historical mentions in `dev/WORKLOG.md` were left untouched; they document
  past states and are not public/operator contracts.

### Edit HackerNoon article toward TLS steganography analysis

Goal:

- Rework the FPS article draft from a product-forward explanation into a
  technical essay about TLS steganography, censorship-filtering escalation and
  why late carrier authentication is a next-step design.
- Replace ASCII diagrams with rendered assets and carry the server placement
  insight into operator docs.

Done:

- Rewrote `articles/free-porn-storage-fps-hackernoon.md` around:
  - filtering escalation from DNS/IP/protocol/SNI to handshake fingerprints,
    active probing and continuous flow analysis;
  - TLS lifecycle and record-layer shape;
  - why FPS moves the detection problem beyond connection startup;
  - FPS architecture, pre-reverse-proxy deployment, key features, costs,
    comparisons and development process.
- Removed inline editor comments from the article.
- Smoothed the article away from short slogan-like paragraphs toward longer
  technical prose, and added the early network-steganography reference to
  Manfred Wolf's "Covert Channels in LAN Protocols" from LANSEC '89.
- Added Balboa as the closest known academic relative in the comparison section,
  while distinguishing its traffic-model rewriting approach from FPS's late
  Zero-RTT, TUN and envelope design.
- Added Graphviz sources and rendered SVG assets:
  - `articles/assets/censorship-filtering-ladder.{dot,svg}`;
  - `articles/assets/fps-architecture.{dot,svg}`;
  - `articles/assets/fps-tls-record-stream.{dot,svg}`.
- Updated `docs/real-origin-carriers.md` with explicit guidance that
  `fps_server` should sit before TLS termination, as a pre-reverse-proxy/TCP
  gate, with optional L4/TCP proxy, TLS passthrough or SNI-router layers after
  it.

Verification:

- `dot -Tsvg ...` for all three article diagrams.
- `rg -n '\{[^}]+\}|Killer Features|Conceptually:' articles/free-porn-storage-fps-hackernoon.md || true`
- `wc -w articles/free-porn-storage-fps-hackernoon.md`
- local Markdown link check for article assets and relative docs.
- `file articles/assets/*.svg`
- `git diff --check`

Decision:

- Use SVG as the article asset format in-repo. HackerNoon can receive exported
  images later, but SVG keeps the source reviewable and compact for now.

## 2026-05-19

### Draft HackerNoon article for FPS

Goal:

- Add a self-contained public article draft that explains FPS to a broader
  technical audience without turning the public docs into marketing copy.

Done:

- Created `articles/free-porn-storage-fps-hackernoon.md`.
- Covered the FPS name rationale, censorship-resistance motivation, current
  architecture, late Zero-RTT upgrade, envelope mode, TUN/carrier model,
  strengths, limitations, related systems, development process, test culture and
  future work.
- Kept the censorship overview generic and avoided explicit country/system
  examples in the article body.
- Used primary/baseline sources for comparison notes: Project X VLESS/XTLS
  docs, Trojan protocol docs, NaiveProxy README, Cloak wiki, Tor pluggable
  transports docs and Shadowsocks AEAD-2022 spec.

Verification:

- `wc -w articles/free-porn-storage-fps-hackernoon.md`
- `git diff --check`

Decision:

- Keep article drafts under `articles/` outside public `docs/`. Public docs
  stay operator-focused; article drafts can use a more narrative style.

### Remove redundant release-candidate workflow

Goal:

- Simplify GitHub Actions to three workflows: required `CI`, scheduled/manual
  `Quality` and manual `Publish Private Images`.
- Avoid maintaining a separate release-candidate workflow whose dry-run
  behavior is already covered by `Publish Private Images` with `publish=false`.

Done:

- Removed `.github/workflows/release-candidate.yml`.
- Updated release/testing/private-GitHub docs to describe `Publish Private
  Images publish=false` as the release-candidate dry run and `publish=true` as
  private GHCR publication.
- Updated `dev/ROADMAP.md`, `dev/REVIEW.md` and the Docker artifact static test
  so the separate release-candidate workflow does not reappear accidentally.

Verification:

- `python3 -m py_compile tests/integration/docker_artifacts.py`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- workflow YAML parse via `python3 - <<'PY' ...`
- `ctest --test-dir build -R fps_docker_artifacts --output-on-failure`
- `rg -n 'Release Candidate|release-candidate workflow|upload_image_artifacts|four GitHub Actions workflows|two manual image workflows' docs dev tests .github README.md`
- `git diff --check`

Decision:

- Keep `Quality` separate for now. It is conceptually distinct from required PR
  CI and cheaper to reason about than one large conditional workflow.

## 2026-05-18

### Simplify private GHCR publish tags

Goal:

- Make private GHCR publication match the operator expectation: two runtime
  image tags per publish run, not a set of variant and technical tags.
- Avoid noisy untagged OCI package versions during private beta experiments.

Done:

- Changed `Publish Private Images` so each run publishes only:
  - `ghcr.io/<owner>/fps:<image_tag-or-short-sha>` for the Ubuntu runtime;
  - `ghcr.io/<owner>/fps:<image_tag-or-short-sha>-alpine` for the Alpine
    runtime.
- Removed variant-prefixed SHA tags and the `-ubuntu` alias.
- Set `provenance: false` and `sbom: false` on `docker/build-push-action` for
  the private beta workflow. Signed images, SBOM and provenance remain future
  release-policy work rather than implicit side effects.
- Updated release/testing/private-GitHub docs and the Docker artifact static
  test for the new tag contract.

Verification:

- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- workflow YAML parse via `python3 - <<'PY' ...`
- `ctest --test-dir build -R fps_docker_artifacts --output-on-failure`
- `git diff --check`

Decision:

- Keep the private beta package UI simple and operator-facing. Re-enable
  provenance/SBOM only after a separate signing/attestation policy review.

### Validate private GHCR publish workflow remotely

Goal:

- Prove the new private GHCR publish path works in GitHub Actions, not only
  locally.
- Keep the experiment private and explicitly tagged.

Done:

- Pushed `develop`, opened PR `#3` and merged it to `main` after required CI
  passed:
  - PR: `https://github.com/mrcatnapper/fps/pull/3`
  - merge commit: `d5ed65e Add private GHCR publish workflow (#3)`
- First PR CI run found one issue: `fps_docker_artifacts` required
  `.github/workflows/publish-private-images.yml` inside the CI Docker build
  context, but `.dockerignore` intentionally excludes `.github`.
- Fixed the static test to validate the workflow when `.github` is present and
  skip that workflow-specific check inside the trimmed Docker context.
- PR CI run `26060018596` passed all required checks:
  `linux-local / gcc`, `linux-local / clang`, `docker-smoke / ubuntu-gcc`,
  `docker-smoke / ubuntu-clang`, `docker-smoke / alpine-gcc`.
- Ran `Publish Private Images` dry-run with `publish=false`:
  - run `26060239051`;
  - Ubuntu and Alpine jobs passed;
  - no GHCR login/push occurred.
- Ran one real private GHCR push with
  `image_tag=ghcr-exp-d5ed65eae20f` and `publish=true`:
  - run `26060397697`;
  - Ubuntu and Alpine jobs passed;
  - pushed tags:
    - `ghcr.io/mrcatnapper/fps:ghcr-exp-d5ed65eae20f`;
    - `ghcr.io/mrcatnapper/fps:ghcr-exp-d5ed65eae20f-ubuntu`;
    - `ghcr.io/mrcatnapper/fps:ghcr-exp-d5ed65eae20f-alpine`;
    - `ghcr.io/mrcatnapper/fps:ubuntu-d5ed65e`;
    - `ghcr.io/mrcatnapper/fps:alpine-d5ed65e`.
- Confirmed unauthenticated local pull fails with `unauthorized`, as expected
  for a private GHCR package.
- Attempting to list package versions through the currently authenticated `gh`
  token failed with HTTP 403 because the token lacks `read:packages`.
- Aligned local and remote `develop` to the squash-merged `main` commit.

Verification:

- `gh pr checks 3 --watch`
- `gh workflow run "Publish Private Images" --ref main ... publish=false`
- `gh run watch 26060239051`
- `gh workflow run "Publish Private Images" --ref main ... publish=true`
- `gh run watch 26060397697`
- GH Actions job logs for pushed tag/digest evidence.
- `docker pull ghcr.io/mrcatnapper/fps:ghcr-exp-d5ed65eae20f` returned
  `unauthorized` without registry auth.

Decision:

- Workflow-based publication is the preferred path for release candidates
  because it records commit SHA, smoke-test result, pushed tags and digests.
- Local/manual pulls need a token with `read:packages`; local/manual pushes need
  `write:packages`. GitHub Actions publication itself works with
  `GITHUB_TOKEN` and workflow `packages: write`.

### Add private GHCR publishing experiment

Goal:

- Start deployment experiments for Docker-first releases by adding a manual
  private GHCR image publishing path.
- Keep publication explicit, private and reversible while the repository is
  still being prepared for recreation.

Done:

- Added `.github/workflows/publish-private-images.yml`.
- The workflow is `workflow_dispatch` only and defaults to `publish=false`,
  which performs a build-only dry run.
- When `publish=true`, it logs in to GHCR with `GITHUB_TOKEN`, runs the Docker
  runtime smoke first, then publishes Ubuntu and Alpine runtime images.
- Tags are explicit and pre-release oriented:
  unsuffixed version tag for Ubuntu, `-ubuntu`/`-alpine` variant tags and
  variant-prefixed short-SHA tags. No `latest` tag is published.
- Workflow permissions are limited to `contents: read` and `packages: write`.
- Updated GitHub Actions to current Node 24 based action majors:
  `actions/checkout@v6`, `docker/setup-buildx-action@v4` and
  `actions/upload-artifact@v6`; the new publish workflow uses
  `docker/login-action@v4`, `docker/metadata-action@v6` and
  `docker/build-push-action@v7`.
- Updated public release/testing/beta-status docs and private GitHub operations
  notes with GHCR workflow behavior, manual local push commands, tags and
  permission guidance.
- Updated the Docker artifact static test to cover the new publish workflow.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- YAML parse of all `.github/workflows/*.yml`
- `ctest --test-dir build -R fps_docker_artifacts --output-on-failure`
- `git diff --check`

Decision:

- Keep image publishing manual and private-only. Signed images, public release
  publishing, `latest` tags and broader `id-token`/attestation permissions stay
  out of scope until release policy is reviewed.

### Prepare docs for GitHub Pages root

Goal:

- Clean and restructure public documentation under `docs/` before using it as
  the GitHub Pages root.
- Keep public/operator pages independent from agent/developer artifacts in
  `dev/`.
- Cross-check the docs against current code, tests and recent review notes so
  stale pre-production claims are not reintroduced.

Done:

- Added `docs/index.md` as the GitHub Pages documentation home page with
  grouped get-started, operations and reference links.
- Reduced `docs/README.md` to a short pointer to `index.md`, avoiding two public
  navigation pages that can drift.
- Renamed public review-style pages:
  `docs/client-ux.md` -> `docs/client-profiles.md` and
  `docs/beta-readiness.md` -> `docs/beta-status.md`.
- Removed public `docs/` links to `dev/` private/developer notes and replaced
  them with public-facing descriptions of CI, protocol-review and release
  gates.
- Updated root README and specification cross-links to the new public document
  names.
- Updated the Docker artifact static test to validate `docs/index.md` and the
  renamed client-profile document.
- Rechecked docs against code/tests for current `fps_carrier origin/client`,
  Docker proxy overlay, UUID `replace_old`, status socket and removed
  pre-production CLI/config references.

Verification:

- Markdown link check over `docs/*.md` and `README.md`.
- `rg` checks for removed public-carrier/legacy terms in public docs.
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `git diff --check`

Decision:

- Keep exact historical rationale and manual run details in `dev/`; public docs
  describe current behavior, operator workflows and beta gates without depending
  on developer-only files.

### Configure private GitHub branch protection

Goal:

- Apply the private-repository branch protection now that `gh` has enough
  permissions.
- Keep the policy strict enough to protect `main`, but not so strict that
  emergency admin maintenance becomes impossible in a one-operator private beta
  phase.

Done:

- Enabled `main` branch protection.
- Required strict `CI` checks:
  `linux-local / gcc`, `linux-local / clang`, `docker-smoke / ubuntu-gcc`,
  `docker-smoke / ubuntu-clang`, `docker-smoke / alpine-gcc`.
- Enabled required pull request review protection with one approving review.
- Left admin enforcement disabled for emergency bypass.
- Disabled force-pushes and branch deletion on `main`.
- Required linear history and conversation resolution.
- Disabled merge commits; kept squash and rebase merge enabled.
- Enabled the update-branch button and kept delete-branch-on-merge disabled
  because `develop` is long-lived.
- Enabled Dependabot vulnerability alerts and automated security fixes.
- Tried to enable secret scanning and push protection; GitHub returned that
  secret scanning is not available for this private repository.
- Updated `dev/PRIVATE_GITHUB_OPERATIONS.md` with the actual policy.

Verification:

- `gh api repos/:owner/:repo/branches/main/protection`
- `gh api repos/:owner/:repo/branches/main/protection/required_pull_request_reviews`
- `gh api repos/:owner/:repo/vulnerability-alerts`
- `gh api repos/:owner/:repo/automated-security-fixes`
- `gh repo view --json ...`
- `git diff --check`

Decision:

- Do not require the scheduled/manual `Quality` workflow for every PR yet; it
  remains a release-candidate/manual gate.
- Do not enable admin enforcement until there is more than one reliable
  maintainer path.
- Keep secret scanning as a manual checklist item until GitHub exposes it for
  this private repository.

### Document project-name rationale and CI push discipline

Goal:

- Explain why the expanded project name is intentionally misleading without
  changing protocol or runtime behavior.
- Add agent rules for local-first work, avoiding unnecessary GitHub CI triggers,
  CI skip markers for documentation-only pushes and safe branch-history cleanup
  on `develop`.
- Run the manual private `Quality` workflow and fix CI-package issues if the
  gate itself is broken.

Done:

- Added short naming rationale to `README.md` and `docs/specification.md`.
- Updated `AGENTS.md` version-control rules: local verification first, no CI
  skipping for source/runtime changes, `--force-with-lease` only on `develop`
  or feature branches when it is safe.
- Updated `dev/PRIVATE_GITHUB_OPERATIONS.md` with GitHub Actions skip markers,
  the required-checks caveat and branch rewrite guidance.
- Triggered GitHub Actions `Quality` run `26004768224`; it failed before tests
  could run under sanitizers because the quality Docker image lacked
  `libclang-rt-20-dev`.
- Added `libclang-rt-20-dev` to the `ci` Docker stage so clang-20
  ASan/UBSan/libFuzzer runtime libraries are present.

Verification:

- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`
- `docker build --target ci --build-arg FPS_COMPILER=clang -t fps:quality-local .`
- `docker run --rm -e FPS_FUZZ_RUNS=64 -e FPS_JOBS=2 fps:quality-local tools/run_quality_checks.sh --all`
- GitHub Actions `Quality` on `develop`: run `26005095594`, success in 10m58s.

Decision:

- The CI package fix touches a build file, so the eventual push must not use
  `[skip ci]`.
- GitHub still annotates workflows with the Node.js 20 deprecation warning for
  `actions/checkout@v4` and `docker/setup-buildx-action@v3`; handle that as a
  separate workflow-maintenance increment.

### Add private release-candidate workflow

Goal:

- Start moving from local-only deployment checks to a private GitHub flow
  without enabling public releases or registry publication yet.
- Build and smoke-test Ubuntu and Alpine runtime images from GitHub Actions on
  demand.

Done:

- Added `.github/workflows/release-candidate.yml` with manual
  `workflow_dispatch` only.
- The workflow builds Ubuntu and Alpine candidate images through the existing
  Docker smoke path.
- Optional `upload_image_artifacts=true` saves `docker save` tarballs as private
  workflow artifacts; default is no artifact upload.
- Updated private GitHub operations notes, roadmap, release docs and testing
  docs.

Verification:

- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`

Decision:

- Keep GHCR publication, signed images and public release tags out of this
  increment. The next step is private GHCR publication after tag/permission
  policy is reviewed.

### Restore detailed worklog and move private GitHub notes to develop

Goal:

- Restore the detailed historical `dev/WORKLOG.md` after the squashed baseline,
  because it remains useful as design rationale even when old commits are no
  longer addressable.
- Move the previous private-GitHub documentation cleanup off `main` and onto a
  `develop` branch.
- Check whether GitHub CLI is available for workflow inspection.

Done:

- Created local branch `develop` at `68d27ed Refresh private GitHub developer
  notes`.
- Moved local `main` back to `origin/main` / `b47de84 Init`, so the previous
  cleanup commit now lives on `develop`.
- Restored the pre-squash detailed `dev/WORKLOG.md` from `b47de84` and kept
  adding new entries at the top.
- Verified `gh` is installed.

Verification:

- `git status --short --branch`
- `git log --oneline --decorate -3 --all`
- `gh --version`
- `gh auth status`

Open:

- `gh auth status` currently reports that no GitHub host is logged in. To let an
  agent inspect workflow runs or dispatch workflows directly, authenticate `gh`
  or provide a suitable `GH_TOKEN`.

## 2026-05-17

### Reduce test boilerplate after source split refactors

Goal:

- Continue low-risk refactoring that reduces source size and duplication without
  changing runtime behavior.
- Start with the largest remaining duplication in tests rather than reshaping
  production contracts again immediately after the relay/session/config splits.

Done:

- Replaced repeated enum-name assertions in `tests/test_enum.cpp` with a local
  one-line check macro.
- Replaced repeated logging severity parse/string assertions in
  `tests/test_logging.cpp` with a small table.
- Added test-only CLI helpers in `tests/test_tcp_relay_app.cpp` for building
  `argc`/`argv` and invoking server/client CLI paths; removed repeated manual
  `std::vector<char*>` setup blocks.
- Applied the repository clang-format style to the touched C++ test files.

Verification:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `git diff --check`

Decision:

- No production behavior, config, protocol, Docker or docs changed in this
  increment. The remaining largest source files are now mostly production CLI
  and runtime code; further reductions should stay mechanical and use the same
  contracts as the live daemons.

### Split relay config shaper parsing

Goal:

- Continue the mechanical refactor of oversized C++ files while preserving one
  source of truth for config validation.
- Reduce `src/net/tcp_relay_config.cpp` without moving validation to external
  scripts.

Done:

- Moved reusable Boost.JSON config access helpers into
  `src/net/tcp_relay_config_helpers.hpp`.
- Moved shaper profile parsing/loading into
  `src/net/tcp_relay_config_shaper.cpp` with private declarations in
  `src/net/tcp_relay_config_shaper.hpp`.
- Left `src/net/tcp_relay_config.cpp` responsible for endpoint parsing,
  Zero-RTT auth config, TUN config and final relay config assembly.
- Updated `fps_linux_runtime` sources in CMake.

Verification:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `git diff --check`

Decision:

- `--check-config` continues to use the same C++ loader and semantic checks as
  the production relay binaries; no separate config-validator script was added.

### Split bridge session IO path

Goal:

- Continue reducing large C++ translation units after the relay CLI/runtime
  split.
- Keep `TcpBridgeSession` behavior and public API unchanged.

Done:

- Moved the async read, Zero-RTT processing, envelope enqueue, write queue and
  shaper path into `src/net/tcp_bridge_session_io.cpp`.
- Added private shared helpers in `src/net/tcp_bridge_session_helpers.hpp` for
  close metadata, bounded size arithmetic, envelope config construction and
  stats helpers.
- Left `src/net/tcp_bridge_session.cpp` focused on construction, lifecycle
  shutdown, accessors and handler emission.
- Updated `fps_core` sources in CMake.

Verification:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`

Decision:

- This is still a mechanical refactor. No protocol, config, CLI or runtime
  contract changed.

### Split relay CLI from relay runtime

Goal:

- Reduce the size and cognitive load of `src/net/tcp_relay_app.cpp` without
  changing relay, config or CLI behavior.
- Keep `--check-config` and profile/lease/status commands backed by the same C++
  parser and semantic validation as the daemons.

Done:

- Moved CLI parsing, profile import/export, status query and lease-management
  commands into `src/net/tcp_relay_cli.cpp`.
- Added private shared relay formatting helpers in
  `src/net/tcp_relay_app_helpers.hpp`.
- Left `src/net/tcp_relay_app.cpp` focused on the async relay runtime, TUN,
  status socket, carrier registration and bridge callbacks.
- Updated `fps_linux_runtime` sources in CMake.

Verification:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `git diff --check`

Decision:

- Do not move config validation into an external script yet. `--check-config`
  is part of the binary/runtime contract and should keep using the exact same
  parser and semantic checks as `fps_client`/`fps_server`.

### Fix post-format test warnings and rerun quality suite

Goal:

- Remove compiler warnings introduced by new `ZeroRttUpgradeConfig` fields in
  test/fuzz helpers after the clang-format pass.
- Rerun the broad regression, sanitizer, coverage, Docker and TUN checks.

Done:

- Added explicit `.replay_cache = nullptr` initializers to Zero-RTT test and
  fuzz helper configs.
- Added focused regression assertions for operational enum names,
  `FpsUpgradeController::next_record_index`, lease allocator accessors and TUN
  pump accessors.
- Restored the coverage quality gate after it dipped below the function
  threshold; latest coverage is 72.20% line and 80.04% function coverage.
- Observed one transient ASan integration failure in
  `fps_https_zero_rtt_large_response`; the targeted repeat and full ASan local
  suite passed immediately afterward.

Verification:

- `cmake --build build --clean-first -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `tools/run_quality_checks.sh --all`
- `tools/run_quality_checks.sh --coverage`
- `tools/run_quality_checks.sh --fuzz`
- `FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `tools/docker_tun_iperf_sim.py --image fps:local --duration 10 --bandwidth 5M --length 1200`
- `tools/docker_socks_smoke.py --image fps:local`
- `tools/docker_multi_client_sim.py --image fps:local --duration 5 --bandwidth 1M --length 512`
- `git diff --check`

Decision:

- Keep the coverage threshold instead of relaxing it; the missing coverage was
  cheap to cover with stable public contract checks.

### Remove third-party public carrier references from user docs

Goal:

- Keep user/operator documentation vendor-neutral and avoid steering beta users
  toward public test services as carrier origins.

Done:

- Replaced Postman/public echo examples in `docs/real-origin-carriers.md` with
  neutral `carrier.example.net` placeholders and operator-controlled origin
  guidance.
- Updated the public beta quickstart, Docker docs, beta readiness and proxy
  overlay docs to describe long-lived TLS sessions, WebSocket streams, long
  polling, keep-alive HTTPS and application clients without naming public echo
  providers.
- Left historical Postman references in `dev/` review/worklog material.

Verification:

- `rg -n -i 'postman|echo\.websocket|websocket\.org|public echo|echo service' docs README.md`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `git diff --check`

Decision:

- User docs should document the carrier mechanics, not advertise third-party
  public services as convenient origins.

### Document real-origin carrier UX

Goal:

- Address the latest Postman-origin UX review without adding generator or
  doctor tooling yet.
- Make real-origin carrier operation, lease-vs-carrier readiness, SOCKS smoke,
  weak-VM deployment and cleanup steps explicit in public docs.
- Check whether a router-hosted `fps_client` entry point is conceptually
  compatible with the current carrier/DNS model.

Done:

- Added `docs/real-origin-carriers.md` with `/etc/hosts`, browser/application
  carrier rules, Python `websockets`, `websocat`, Postman GUI and
  `curl --resolve` examples.
- Updated the public beta quickstart to separate deterministic `fps_carrier`
  debug flow from real-origin carrier sessions and to call out
  `sessions.carriers_current > 0` as required readiness.
- Updated Docker docs with weak-server `docker save | ssh docker load` guidance
  and carrier-liveness troubleshooting.
- Expanded proxy overlay docs with SOCKS smoke commands, expected Dante log
  signals and stronger warnings around unauthenticated SOCKS defaults.
- Documented router/LAN-gateway DNS override as an experimental pattern:
  `fps_client` listens on the router LAN address, router DNS maps selected
  carrier hostnames to that address, and LAN devices keep ordinary carrier URLs.
- Updated beta readiness, roadmap, review and UX flow notes to keep
  `fps_deploy init`, `doctor`, status field renames and supported OpenWrt
  claims deferred.

Verification:

- `rg -n 'OpenWrt|router|LAN gateway|dnsmasq|carrier hostnames|carriers_current|real-origin|SOCKS auth' docs dev README.md`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`

Decision:

- Postman Echo is documented only as a manual beta smoke target, not as a
  production carrier recommendation.
- Router-hosted client mode is documented as plausible but unvalidated; it needs
  real OpenWrt/router hardware or close VM testing before it is supported.

### Rerun real-origin manual carrier deployment flow

Goal:

- Rerun the full two-host deployment flow after UX/bug-fix work using a real
  public origin and no `fps_carrier.py`.
- Act as an average DevOps user: deploy server/client, manually open carrier
  sessions, exercise SOCKS/TUN user traffic, collect UX feedback, rewrite
  `dev/UX_FLOW_REVIEW.md`, then clean up runtime artifacts.

Done:

- Selected `wss://ws.postman-echo.com/raw` as the real carrier origin after a
  direct WebSocket echo preflight.
- Built fresh `fps:local` and `fps-dante-proxy:local` images from the current
  tree; loaded both onto `fpshop` via `docker save ... | ssh fpshop docker load`.
- Created clean runtime directories outside the repo:
  `/tmp/fps-postman-flow` locally and `/root/fps-postman-flow` remotely.
- Generated a fresh server keypair and client UUID; generated the client config
  with `fps_server --generate-client-profile`.
- Started remote `fps-server` on public host port `443` with origin
  `ws.postman-echo.com:443`, TUN `fpss0`, lease pool `10.66.0.0/30`, and Dante
  SOCKS5 overlay on `10.66.0.1:1080`.
- Started local `fps-client` on `127.0.0.1:443`, TUN `fpsc0`,
  `auto_configure=true`.
- Opened manual Postman WebSocket carrier sessions by connecting TCP to
  `127.0.0.1:443` while preserving SNI/Host/URL
  `wss://ws.postman-echo.com/raw`; no `fps_carrier.py` command was used.
- Confirmed a one-shot manual carrier authenticates and assigns a lease but
  leaves `carriers_current=0` after it closes, so TUN traffic does not work
  unless a carrier remains open.
- Held three concurrent manual WebSocket carriers; status showed
  `carriers_current=3`, `auth.authenticated=8`, and zero auth/envelope failures.
- Exercised TUN and SOCKS user scenarios:
  `ping 10.66.0.1`, `curl --socks5-hostname 10.66.0.1:1080` to ipify,
  example.com and Postman Echo GET/POST.
- Reviewed local/server/SOCKS logs; no envelope/codec/TLS parse/tcp read error
  signature was found.
- Cleaned up runtime artifacts: stopped manual carrier process, ran local and
  remote compose `down -v --remove-orphans`, removed both runtime directories,
  verified no `fps-postman*` containers, no port `443` listeners and no
  `fpsc0`/`fpss0` devices remained.
- Rewrote `dev/UX_FLOW_REVIEW.md` for the Postman real-origin flow and DevOps
  user perspective.

Verification:

- Direct origin preflight:
  `python3` + `websockets.connect("wss://ws.postman-echo.com/raw")`
- `docker build -t fps:local .`
- `docker build -f examples/docker/proxy-dante/Dockerfile --build-arg FPS_BASE_IMAGE=fps:local -t fps-dante-proxy:local .`
- `docker save fps:local fps-dante-proxy:local | ssh fpshop docker load`
- `docker run --rm -v /tmp/fps-postman-flow/server/config:/etc/fps:ro fps:local fps_server --check-config --config /etc/fps/server.json`
- `docker run --rm -v /tmp/fps-postman-flow/client/config:/etc/fps:ro fps:local fps_client --check-config --config /etc/fps/client.json`
- `ssh fpshop 'cd /root/fps-postman-flow/server && docker run --rm -v /root/fps-postman-flow/server/config:/etc/fps:ro fps:local fps_server --check-config --config /etc/fps/server.json'`
- `ssh fpshop 'cd /root/fps-postman-flow/server && docker compose --profile socks up -d'`
- `cd /tmp/fps-postman-flow/client && docker compose up -d`
- Manual WebSocket carrier script using Python `websockets`, TCP
  `127.0.0.1:443`, URL/SNI/Host `wss://ws.postman-echo.com/raw`.
- `cd /tmp/fps-postman-flow/client && docker compose run --rm --no-deps fps-client status`
- `ssh fpshop 'cd /root/fps-postman-flow/server && docker compose --profile socks run --rm --no-deps fps-server status'`
- `ping -c 3 -W 2 10.66.0.1`
- `curl --socks5-hostname 10.66.0.1:1080 -fsS https://api.ipify.org?format=json`
- `curl --socks5-hostname 10.66.0.1:1080 -fsS -I https://www.example.com/`
- `curl --socks5-hostname 10.66.0.1:1080 -fsS 'https://postman-echo.com/get?fps=socks-flow'`
- Log grep on client/server for envelope, codec, TLS parse, queue and TCP read
  error signatures.
- Cleanup verification:
  no `fps-postman*` containers, `/tmp/fps-postman-flow` and
  `/root/fps-postman-flow` removed, port `443` free locally/remotely, `fpsc0`
  and `fpss0` gone.

Decisions:

- Use SOCKS for real-user browsing/fetch scenarios instead of applying
  host-wide split/default routes.
- Leave Docker images/build cache in place after cleanup; remove runtime
  containers, volumes, directories and TUN devices.
- Treat public Postman Echo as a useful real-origin smoke target, not a
  production carrier recommendation.

Open questions:

- Should docs include a first-class real-origin manual carrier guide that does
  not rely on `fps_carrier.py`.
- Should status/doctor explicitly warn when a lease exists but
  `carriers_current=0`.
- Should deployment tooling generate the two-host runtime bundle and image
  transfer commands for weak VMs.

### Prepare private GitHub handoff

Goal:

- Close the next beta-readiness process gap without publishing releases or
  changing runtime behavior.
- Make the first private GitHub push, first CI run and protocol-review handoff
  repeatable.

Done:

- Added `dev/PRIVATE_GITHUB_HANDOFF.md` with pre-push checks, artifact/secret
  scans, first CI run expectations and protocol review packet contents.
- Extended `.gitignore` for local pcaps, status sockets, coverage profiles and
  lease runtime files.
- Updated beta/testing/release docs to point at the private handoff checklist.
- Clarified protocol-review language: the adversarial replay test covers
  captured-prefix replay, not natural TLS-record collision.
- Updated roadmap/review notes to show the remaining beta gates as external
  protocol review, first remote CI validation, repeated release-candidate soak
  and onboarding feedback.

Verification:

- `git ls-files | rg -n '(\\.pcap|\\.pcapng|\\.cap|\\.profraw|\\.profdata|\\.status|\\.sock|leases\\.json|captures/)'`
- `rg -n 'server_private_key_base64|client_uuid|fps://v1|BEGIN.*PRIVATE KEY|PRIVATE KEY-----' --glob '!docs/**' --glob '!dev/**' --glob '!examples/**' --glob '!tests/**' --glob '!tools/**' --glob '!src/**' --glob '!include/**'`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`

Decision:

- Keep publishing/signing/upstream work deferred; this increment only prepares
  the private repository handoff and review evidence.

### Add adversarial auth status coverage

Goal:

- Make the beta status surface more actionable for Zero-RTT/auth failures and
  envelope failures without preserving pre-release field compatibility.
- Add a live local adversarial regression for no-upgrade passthrough, unknown
  clients, captured-prefix replay and post-auth envelope tamper.

Done:

- Status JSON now exposes root `auth` and `envelope` counter groups. Auth is no
  longer duplicated under the session lifecycle group.
- Relay increments auth counters for candidates, successful authentication,
  precheck misses, unknown clients, decrypt failures, replay rejects and
  confirmation failures.
- Relay increments envelope counters for encoded/decoded records and
  encode/decode/tamper failures.
- Zero-RTT replay cache is now shared across session engines created from one
  loaded relay config, so replaying a valid upgrade on a different TCP
  connection is rejected by the same daemon process.
- Added `fps_https_zero_rtt_adversarial`, a local integration test that uses
  live daemons, status sockets and a small recording/tampering proxy.
- Updated public docs and developer review notes for the new status JSON shape
  and the daemon-process replay-cache policy.

Verification:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build -R fps_https_zero_rtt_adversarial --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`

Decisions:

- No compatibility layer is kept for the old session-group authenticated
  counter; pre-release status consumers should use `auth.authenticated`.
- Replay persistence across daemon restarts remains future hardening and should
  be revisited after independent protocol review.

### Translate agent instructions and trim Docker artifact checks

Цель:

- Сделать корневой `AGENTS.md` пригодным для будущих агентов и сторонних
  разработчиков без ручного перевода.
- Уменьшить хрупкость `tests/integration/docker_artifacts.py`, убрав
  дублирующие проверки внутренностей Dockerfile'ов.

Сделано:

- `AGENTS.md` переведен на английский с сохранением текущих project rules:
  docs/dev layout, Git workflow, testing expectations, dependency policy,
  sudo/network safety and Pareto-oriented planning.
- `fps_docker_artifacts` больше не проверяет длинные списки package/stage/body
  strings в `Dockerfile`, `Dockerfile.alpine` и Dante overlay Dockerfile.
- Для Dockerfile'ов оставлены легкие guardrails: файл должен читаться, не
  содержать secret markers, а base FPS images не должны снова включать embedded
  Dante/SOCKS port/entrypoint.

Проверка:

- `python3 -m py_compile tests/integration/docker_artifacts.py`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`
- `ctest --test-dir build -R fps_docker_artifacts --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`

Решения:

- Содержимое Dockerfile'ов считается покрытым фактическими Docker build/smoke
  проверками; static artifact test должен проверять продуктовые контракты,
  примеры и документацию, а не быть копией build files.

### Prepare protocol review brief and validate rotation drill

Цель:

- Закрыть ближайшие функциональные beta-gate пункты без release publishing:
  protocol review package и практическую проверку UUID/server-key rotation.
- Не добавлять GitHub release/GHCR/signing workflows, потому publishing
  отложен до настройки upstream и стабилизации кода после ревью.

Сделано:

- Добавлен `dev/PROTOCOL_REVIEW_BRIEF.md` для внешнего ревью: threat model,
  Zero-RTT indexed precheck, envelope mode, replay/cache policy, lease
  routing/isolation, logging/status secrecy, evidence и known beta risks.
- `tests/integration/docker_artifacts.py` теперь статически проверяет наличие
  protocol review brief и ключевых review topics.
- Обновлен `docs/beta-readiness.md`: protocol review package и rotation drill
  отражены как готовые артефакты; publishing/signing явно оставлены deferred.
- Обновлены `docs/rotation.md` и `dev/ROADMAP.md`: добавлен validation drill и
  политика повторять его для release candidates или после изменений tooling.
- Проведен временный local Docker rotation drill на образе `fps:rotation-drill`
  без добавления orchestration script в репозиторий:
  - old generated profile authenticated and passed a short UDP TUN probe;
  - after allowlist update, `--lease-revoke-client-uuid` and `--lease-prune`,
    stale old profile no longer authenticated;
  - regenerated profile for the new UUID authenticated and passed the same UDP
    TUN probe;
  - after server keypair rotation, the stale profile with old
    `server_public_key_base64` no longer authenticated;
  - regenerated profile with the new server public key authenticated and passed
    the probe.

Проверка:

- `docker build -t fps:rotation-drill .`
- Temporary local Docker rotation drill:
  `python3 - <<'PY' ... PY`
  - final result: `{"result":"ok","old_uuid_rejected":true,"old_server_public_key_rejected":true,"new_uuid_authenticated":true,"rotated_server_key_authenticated":true}`
  - UDP probe phases reported about `200Kbit/s` and `0%` loss.
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`

Решения:

- Keep rotation as explicit restart/redeploy operator procedure for beta; no
  hot reload or management API in this increment.
- Profile files generated by root-running Docker containers remain `0600`;
  use host redirection, `--user "$(id -u):$(id -g)"` or explicit `chown` for
  bind-mount ownership.
- No publishing/signing/upstream-dependent work in this increment.

### Add public beta onboarding and release hygiene docs

Цель:

- Закрыть самые практичные 80/20 пункты Public Beta Gate после успешного
  two-host soak: onboarding, rotation runbooks и минимальная release hygiene.
- Не добавлять новые deploy-helper scripts до получения beta feedback.

Сделано:

- Добавлен `LICENSE` с MIT license.
- Добавлен `docs/public-beta-quickstart.md`: Docker-first flow для server,
  self-hosted carrier, generated client profile/URI, client host-network compose,
  `/etc/hosts` carrier mapping, status verification и optional Dante overlay.
- Добавлен `docs/rotation.md`: reissue/revoke client UUID, lease revoke/prune,
  server key rotation и явное отсутствие config hot reload.
- Добавлен `docs/release.md`: preflight, CI/quality, Docker image checks,
  two-host soak repeat, beta tag shape и rollback checklist.
- Обновлены `README.md`, `docs/README.md`, `docs/docker.md`,
  `docs/client-ux.md`, `docs/linux-routing.md`, `docs/beta-readiness.md` и
  `dev/ROADMAP.md`, чтобы новая документация стала основным operator path и
  чтобы gate/status отражали MIT license и documented rotation workflow.
- `tests/integration/docker_artifacts.py` расширен статическими проверками на
  license, quickstart, rotation/release docs и отсутствие возврата к public echo
  или embedded proxy-daemon в base image.

Проверка:

- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `rg -n 'echo\\.websocket\\.org|hold-wss|embedded SOCKS|shared UUIDs are supported|group UUIDs are supported' docs README.md examples || true`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `git diff --check`

Решения:

- Public beta license: MIT.
- Quickstart остается документацией, а не новым magic script.
- Config reload, signed images, release publishing, privileged CI and
  independent protocol review остаются отдельными gate items.

### Run two-host public beta soak

Цель:

- Закрыть Public Beta soak gate нормальным distributed-прогоном, а не только
  single-host Docker compose smoke.
- Использовать доступный по SSH слабый сервер `fpshop` как remote server host.
- Не добавлять в репозиторий одноразовый двухмашинный orchestration script.

Сделано:

- Собран текущий Docker runtime image `fps:soak-306ae34` из коммита
  `306ae34`.
- Image загружен на `fpshop` через `docker save | gzip | ssh fpshop docker load`.
- Поднят временный двухмашинный сценарий:
  - remote `fpshop`: `fps_server` + self-hosted `fps_carrier origin`;
  - local host: два `fps_client` с разными UUID + два `fps_carrier client`;
  - public carrier path: remote Docker published `443:8443`;
  - TUN pool: `10.90.0.0/29`, server `10.90.0.1`, leased clients
    `10.90.0.2` and `10.90.0.3`.
- 60-second pilot passed first; initial attempt on port `18443` was blocked by
  UFW on `fpshop`, so the test was moved to the already allowed `443/tcp`.
- Main 30-minute soak passed:
  - client-to-server UDP: 1800 seconds, `200K`, 512-byte datagrams,
    87,891 packets, 0 lost, 0.0% loss;
  - concurrent HTTP probe: 3,270 successful requests, no last error;
  - server-to-client UDP to both leased clients: 30 seconds each, 0 lost,
    0.0% loss;
  - spoofed source `10.90.0.6` was dropped and status reported
    `ignored_spoofed_tun_source=1`;
  - valid UDP after spoof passed with 0.0% loss;
  - carrier-client-a stop/start recovery passed; `no_carrier_session`
    incremented and post-recovery UDP passed with 0.0% loss;
  - all six services were running before cleanup;
  - status snapshots were checked for absence of UUID/key/ClientID fields.
- Temporary compose projects and remote `/tmp/fps-twohost-*` directories were
  removed after the run.
- `docs/beta-readiness.md`, `docs/testing.md`, `dev/ROADMAP.md` and
  `dev/REVIEW.md` updated with the distributed soak result and repeat policy.

Проверка:

- `docker build -t fps:soak-306ae34 .`
- `docker save fps:soak-306ae34 | gzip -1 | ssh fpshop 'gunzip | docker load'`
- Temporary two-host pilot:
  `python3 /tmp/fps_two_host_soak.py --image fps:soak-306ae34 --duration 60 --bandwidth 200K --length 512`
- Temporary two-host main soak:
  `python3 /tmp/fps_two_host_soak.py --image fps:soak-306ae34 --duration 1800 --bandwidth 200K --length 512`
- Cleanup/liveness spot checks:
  `docker ps` locally and over `ssh fpshop`, plus remote `ss -ltnp` for `:443`.

Решения:

- Public Beta long soak gate is closed for the current candidate by this
  successful two-host run.
- Keep repeating a comparable two-host soak per release candidate until a
  privileged scheduled runner exists.
- Do not commit the ad hoc two-host orchestration script unless it becomes
  stable product tooling.

### Stabilize strict soak diagnostics

Цель:

- Проверить, можно ли считать Docker strict backpressure soak beta gate.
- Исправить error-path `tools/docker_resilience_soak.py`.
- Добавить детерминированный regression-сигнал для lease-aware backpressure без
  зависимости от Docker timing.

Сделано:

- Строгий запуск `--require-backpressure-event` проверен: обычный Docker
  bandwidth не гарантирует `write_queue_full`, поэтому этот режим оставлен
  diagnostic/manual, а не promoted gate.
- `docker_resilience_soak.py` исправлен: error path снова имеет `sys` и не
  маскирует исходную ошибку вторичным `NameError`.
- `fps_docker_artifacts` теперь статически проверяет resilience soak contract,
  включая strict флаг, error-path и compose cleanup.
- Unit coverage расширен: server-side lease routing при полной очереди владельца
  lease возвращает `write_queue_full` и не отправляет пакет другому клиенту.
- `docs/testing.md`, `docs/beta-readiness.md` и `dev/ROADMAP.md` уточняют:
  отсутствие Docker-level `write_queue_full` в routine soak допустимо, если
  liveness/routing/recovery checks проходят.

Проверка:

- `tools/docker_resilience_soak.py --duration 2 --bandwidth 200K --length 256 --clients 2 --stress-backpressure --require-backpressure-event --stress-bandwidth 5M`
  - Ожидаемо подтвердил, что strict `write_queue_full` не воспроизводится
    детерминированно в текущей Docker topology.
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=session_manager --catch_system_errors=no`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `tools/docker_resilience_soak.py --duration 2 --bandwidth 200K --length 256 --clients 2 --stress-backpressure --stress-bandwidth 2M`
- `git diff --check`

Решения:

- Не делать strict Docker queue overflow обязательным Public Beta gate.
- Public Beta soak gate остается про liveness, lease routing, spoof-drop,
  carrier recovery, sustained UDP/TCP и отсутствие unexpected protocol/runtime
  errors; строгий queue overflow покрывается unit/integration diagnostics.

This logical commit: `Stabilize strict soak diagnostics`.

## 2026-05-16

### Add Docker resilience soak gate

Цель:

- Закрыть следующий functional beta gate: сделать повторяемый Docker/TUN
  resilience smoke для carrier loss/recovery, spoof-drop liveness, mixed
  UDP/TCP traffic и queue-pressure signals.
- Оставить длинный soak opt-in/manual до появления privileged CI runner.

Сделано:

- Добавлен `tools/docker_resilience_soak.py`: compose-based сценарий с одним
  FPS server, двумя leased FPS clients, self-hosted `fps_carrier`,
  status-socket assertions, UDP `iperf3`, concurrent TCP/HTTP probe,
  carrier stop/start recovery, spoofed-source negative probe и optional Dante
  overlay.
- `tools/run_quality_checks.sh` получил `--soak-smoke` и env-настройки
  `FPS_DOCKER_SOAK_*`.
- `docs/testing.md`, `docs/beta-readiness.md` и `dev/ROADMAP.md` обновлены:
  короткий smoke считается доступным, public beta всё ещё требует серии
  длинных прогонов.

Проверка:

- `python3 -m py_compile tools/docker_resilience_soak.py tools/docker_multi_client_sim.py tools/docker_tun_iperf_sim.py tools/docker_socks_smoke.py`
- `bash -n tools/run_quality_checks.sh`
- `tools/docker_resilience_soak.py --help`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `tools/docker_resilience_soak.py --duration 2 --bandwidth 200K --length 256 --clients 2`
- `tools/docker_resilience_soak.py --duration 2 --bandwidth 200K --length 256 --clients 2 --stress-backpressure --stress-bandwidth 2M`
- `FPS_DOCKER_SOAK_BUILD=0 FPS_DOCKER_SOAK_DURATION=2 FPS_DOCKER_SOAK_BANDWIDTH=200K FPS_DOCKER_SOAK_LENGTH=256 tools/run_quality_checks.sh --soak-smoke`
- `tools/docker_resilience_soak.py --duration 1 --bandwidth 100K --length 256 --clients 2 --with-socks-overlay`
- `git diff --check`

Решения:

- Не добавлять soak в default `ctest` и PR CI: он требует Docker, TUN,
  `NET_ADMIN` и заметное wall-clock время.
- По умолчанию `--soak-smoke` запускает короткий Docker/TUN resilience run с
  non-strict backpressure stress. Строгое требование `write_queue_full`
  включается отдельно через `FPS_DOCKER_SOAK_REQUIRE_BACKPRESSURE=1` /
  `--require-backpressure-event`, чтобы не делать smoke зависимым от
  нестабильной saturation-настройки. Dante overlay включается только через
  `FPS_DOCKER_SOAK_WITH_SOCKS=1` или `--with-socks-overlay`.

This logical commit: `Add Docker resilience soak gate`.

### Enforce duplicate UUID replace_old

Цель:

- Закрыть functional beta gap: один `client_uuid` не должен работать как общий
  L3 identity для нескольких устройств.
- Реализовать единственную поддержанную политику duplicate UUID:
  `replace_old`, где новый active instance для того же UUID/lease вытесняет
  старые carriers.

Сделано:

- Добавлен runtime-only `client_instance_id`: `fps_client` генерирует случайный
  16-byte id на старте процесса и отправляет его только в encrypted post-auth
  control metadata для TUN carriers.
- Добавлен encrypted control payload для client instance metadata; config,
  generated profiles, lease file и status/log output не получают нового
  user-facing поля и не выводят raw instance id.
- Server-side carrier registration с lease pool теперь ждет client instance
  metadata, хранит lease IP + instance id, разрешает несколько carriers одного
  process instance и заменяет старые carriers при другом instance id на той же
  lease.
- Replacement удаляет старые carriers из routing pool, закрывает старые
  sessions и пишет non-secret `event=duplicate_client_replaced`; status получил
  `sessions.duplicate_client_replacements`.
- Добавлен `tools/docker_duplicate_uuid_sim.py`: staged Docker scenario с двумя
  client containers на одном UUID, проверкой shared lease, replacement event,
  status counter, old-client server-to-client block и new-client liveness.
- Unit coverage расширен для same-instance multi-carrier, different-instance
  replacement, old inbound drop, client instance control payload и config
  validation вокруг minimal TUN control metadata size.
- Документы обновлены: specification, client UX, beta readiness, Docker/testing
  docs, roadmap, UX flow review и README.

Проверка:

- `cmake --build build -j 2`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `./build/fps_unit_tests --run_test=session_manager,tun_lease,tcp_relay_app,tcp_bridge_session --catch_system_errors=no`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `tools/docker_duplicate_uuid_sim.py --build --startup-timeout 90`
- `tools/docker_multi_client_sim.py --startup-timeout 90`
- `git diff --check`

Решения:

- Не добавлять config/profile field для instance id и не менять public lease
  file format.
- Не добавлять group/shared UUID mode; continuous reconnect fights between two
  processes on one UUID остаются operator misconfiguration, но simultaneous L3
  carrier sharing больше не происходит.
- Replacement считается non-secret operational event; UUIDs, keys, ClientID,
  raw instance id и payload bytes не логируются.

This logical commit: `Enforce duplicate UUID replace_old`.

### Refresh beta readiness before private GitHub push

Цель:

- Пересмотреть beta readiness после удаления proxy daemons из base image,
  появления proxy overlays и фиксации `/etc/hosts`/UUID policy.
- Подготовить документ к ближайшему private GitHub push и последующим release
  preparation итерациям.

Сделано:

- `docs/beta-readiness.md` переписан как актуальный gate-документ:
  current ready state, private GitHub gate, beta risks, public beta gate и
  deferred-after-beta.
- Зафиксировано, что base Docker image остается FPS-only, Dante является
  derivative overlay example, Squid - documented pattern, а remote GitHub CI
  needs first run after private push.
- В public beta gate добавлены license decision, signed Docker images,
  root/TUN CI, duplicate UUID `replace_old` enforcement and long soak.

Проверка:

- `rg -n 'Dante|Squid|private GitHub|replace_old|DNS proxy|fps-socks|--profile socks|WireGuard|OpenVPN' docs/beta-readiness.md docs README.md`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`

### Move proxy daemons out of the base Docker image

Цель:

- Сузить базовый Docker runtime до FPS binaries/runtime/debug tools без
  встроенного Dante.
- Оставить application proxy поверх FPS TUN как overlay pattern: официальный
  example через derivative Dante image, Squid только как documented HTTP proxy
  pattern.

Сделано:

- Удалены `dante-server`, `fps-socks-entrypoint.sh` и `EXPOSE 1080/tcp` из
  Ubuntu/Alpine base Dockerfiles; CMake больше не устанавливает SOCKS entrypoint.
- `examples/docker/server/compose.yml` больше не содержит `socks` profile.
- Добавлен `examples/docker/proxy-dante`: derivative Dockerfile,
  `fps-dante-entrypoint.sh` и compose overlay, который подключается через
  `network_mode: "service:fps-server"`.
- `tools/docker_socks_smoke.py` теперь строит/использует отдельный
  `fps-dante-proxy:local` image и проверяет именно overlay example.
- Обновлены Docker artifact checks, quality Docker smoke, CI local shell checks
  и docs. WireGuard/OpenVPN не добавлялись в proxy overlay docs; Squid описан
  только как HTTP proxy pattern, официальный compose example оставлен для Dante.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `FPS_DOCKERFILE=Dockerfile.alpine FPS_DOCKER_IMAGE=fps:alpine tools/run_quality_checks.sh --docker`
- `tools/docker_socks_smoke.py --build`
- `git diff --check`

### Document carrier hosts mapping and UUID sharing policy

Цель:

- Зафиксировать результат обсуждения DNS/UX: в beta не добавлять магический DNS
  proxy, а документировать явный `/etc/hosts` override для carrier hostname.
- Зафиксировать продуктовую политику UUID: один `client_uuid` на
  device/profile, shared/group UUID не поддерживается, единственная duplicate
  policy - `replace_old`.

Сделано:

- `docs/client-ux.md` получил разделы про carrier hostname mapping и UUID
  sharing policy.
- `docs/linux-routing.md`, `docs/docker.md`, `README.md` и
  `docs/specification.md` обновлены с практическими ограничениями:
  DNS/hosts не выбирают TCP port, loopback URL ломает browser origin/SNI/CORS,
  DoH/internal resolvers can bypass hosts, DNS proxy не является текущей
  beta-фичей.
- `docs/specification.md`, `docs/beta-readiness.md`,
  `dev/UX_FLOW_REVIEW.md` и `dev/ROADMAP.md` теперь явно говорят, что shared
  UUID не является L3 VPN mode; перед public beta нужно enforce/test
  `replace_old` для duplicate UUID.

Проверка:

- `git diff --check`
- `rg -n 'DNS proxy|/etc/hosts|replace_old|shared UUID|group UUID|per-device' docs dev README.md`

### Refresh public docs after self-hosted carrier UX

Цель:

- Актуализировать публичные docs после удаления public echo/hold flow,
  перехода на self-hosted `fps_carrier origin/client` и агрегации шумных логов.

Сделано:

- Обновлены `docs/beta-readiness.md` и `docs/client-ux.md`: текущий controlled
  beta статус, self-hosted carrier, generated profiles/URI, status socket,
  Docker UX и оставшиеся public beta gates.
- Обновлены `docs/docker.md`, `docs/testing.md` и `docs/specification.md`:
  убраны устаревшие формулировки, WSS-only утверждение, старые coverage numbers
  и упоминания неактуальных auth/deploy paths.
- Public docs теперь описывают текущую схему: Zero-RTT-only auth, UUID client
  identity, inline server base64 keys, self-hosted carrier origin, Docker-first
  runtime, status counters and time-aggregated noisy logs.

Проверка:

- `rg` stale-term scan over public docs and root README.
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`

### Remove public echo carrier UX and aggregate noisy logs

Цель:

- Убрать зависимость UX/tests/docs от public `echo.websocket.org`.
- Вернуть FPS carrier UX к self-hosted `fps_carrier origin/client` с
  самоподписанным TLS и минимальной настройкой.
- Снизить шум `write_queue_full`/pre-carrier логов под throughput через
  time-based repeater.

Сделано:

- `fps_carrier hold`-режим и presets удалены; `fps_carrier origin` теперь
  одновременно отвечает на обычный HTTPS `GET /` и WSS Upgrade.
- `fps_carrier client` проверяет exact WSS echo и больше не принимает отдельный
  server-side bitrate option.
- Local regression заменён на self-hosted carrier HTTP/WSS flow:
  direct HTTPS/WSS carrier tool test и Zero-RTT browser-style HTTPS request
  через `fps_client -> fps_server -> fps_carrier origin`.
- Count-based noisy log limiter заменён на `fps::log::Repeater`, который
  агрегирует повторяющиеся логи примерно раз в 10 секунд; status counters
  остаются точными.
- Обновлены README, Docker/testing docs, Docker smoke/artifact checks и
  `dev/UX_FLOW_REVIEW.md`.

Проверка:

- `python3 -m py_compile tools/fps_carrier.py tests/integration/carrier_tool.py tests/integration/carrier_http_zero_rtt.py tests/integration/fps_https_harness.py`
- `python3 tests/integration/carrier_tool.py --carrier /workspaces/tools/fps_carrier.py`
- `cmake --build build -j 2`
- `ctest --test-dir build -R 'fps_unit_tests|fps_carrier_tool|fps_wss_passthrough|fps_wss_zero_rtt_chain|fps_carrier_http_zero_rtt|fps_docker_artifacts' --output-on-failure`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `tools/docker_tun_iperf_sim.py --duration 5 --bandwidth 1M --length 512`
- `git diff --check`

Решения:

- Не поддерживать compatibility layer для удаленного public hold command.
- Не использовать third-party public echo endpoint как documented default.
- Оставить `fps_carrier` debug/regression utility, а не traffic-shaping model.

### Review echo follow-up logs and stop services

Цель:

- Проверить состояние clean `echo.websocket.org` deployment после
  пользовательских тестов, пересмотреть логи, дополнить `dev/UX_FLOW_REVIEW.md`
  и остановить FPS client/server plus helper services без очистки runtime
  директорий и контейнеров.

Сделано:

- Проверены статусы локальных сервисов `/tmp/fps-echo-flow/client` и удаленных
  сервисов `/root/fps-echo-flow/server` на `fpshop` перед остановкой.
- Локальный клиент был up около 51 минуты:
  `authenticated=10`, `carriers_current=1`, `carriers_registered=10`,
  `carriers_removed=8`; последний authenticated close был `peer_eof`.
- Удаленный сервер был up около 51 минуты:
  `authenticated=10`, `carriers_current=1`, `carriers_registered=10`,
  `carriers_removed=9`, `leases_persisted=1`.
- Просмотрены FPS/client/server/SOCKS/helper logs. Прежние признаки разрушения
  carrier-сессий не обнаружены: нет `envelope_encode_error`,
  `event=envelope_error`, `tls_record_error`, `codec_error`,
  `tls_parse_error`; authenticated sessions закрывались как `peer_eof`.
- Зафиксированы follow-up UX issues:
  public `echo.websocket.org` периодически закрывает long-lived WSS carrier
  примерно после 61 echoed messages при `0.1` rps, helper reconnect работает;
  под throughput остаются шумные bursts `tun_write_rejected
  error=write_queue_full`.
- Остановлены локальные `fps-client` и `echo-carrier-hold`.
- Остановлены remote `fps-server` и `fps-socks` на `fpshop`.
- `dev/UX_FLOW_REVIEW.md` дополнен follow-up review и обновленным runtime
  состоянием: сервисы остановлены, runtime folders/containers оставлены.

Проверка:

- `cd /tmp/fps-echo-flow/client && docker compose ps`
- `cd /tmp/fps-echo-flow/client && docker compose run --rm --no-deps fps-client status`
- `ssh fpshop 'cd /root/fps-echo-flow/server && docker compose --profile socks ps'`
- `ssh fpshop 'cd /root/fps-echo-flow/server && docker compose run --rm --no-deps fps-server status'`
- `docker logs fps-echo-client-fps-client-1`
- `docker logs fps-echo-client-echo-carrier-hold-1`
- `ssh fpshop 'docker logs fps-echo-fps-server-1'`
- `ssh fpshop 'docker logs fps-echo-fps-socks-1'`
- `cd /tmp/fps-echo-flow/client && docker compose stop`
- `ssh fpshop 'cd /root/fps-echo-flow/server && docker compose --profile socks stop'`
- `cd /tmp/fps-echo-flow/client && docker compose ps -a` showed
  `fps-echo-client-fps-client-1` and
  `fps-echo-client-echo-carrier-hold-1` as `Exited`.
- `ssh fpshop 'cd /root/fps-echo-flow/server && docker compose --profile socks ps -a'`
  showed `fps-echo-fps-server-1` and `fps-echo-fps-socks-1` as `Exited`.

Решения:

- Не чистить runtime folders/containers, как просил пользователь.
- Считать follow-up прогон условно успешным: открытые issues относятся к
  observability/noise и public-origin lifetime, а не к подтвержденному FPS
  transport failure.

Незакрытые вопросы:

- Нужно ли расширить `fps_carrier hold-wss` логированием websocket close
  code/reason и reconnect/RTT summary.
- Нужно ли сильнее агрегировать `tun_write_rejected/write_queue_full` логи под
  sustained throughput.

### Rerun clean echo.websocket.org deployment UX flow

Цель:

- Повторить полный чистый operator flow после UX/bug-fix инкрементов:
  поднять FPS server на `fpshop`, локальный FPS client на `127.0.0.1:443`,
  использовать origin `echo.websocket.org:443`, включить server-side SOCKS5,
  выполнить smoke-проверку и переписать `dev/UX_FLOW_REVIEW.md`.

Сделано:

- Очищены старые runtime artifacts: локальные `/tmp/fps-*` пути, удаленный
  `/root/fps-REDACTED`, старые remote compose service/volume state.
- Собран актуальный `fps:alpine`, загружен на `fpshop` через
  `docker save fps:alpine | ssh fpshop docker load`.
- Сгенерированы новые runtime-only secrets/configs вне репозитория:
  `/tmp/fps-echo-flow/client` локально и `/root/fps-echo-flow/server` на VM.
- Сервер запущен через compose profile `socks`: container listen
  `0.0.0.0:8443`, host publish `443`, origin `echo.websocket.org:443`,
  TUN `fpss0`, lease pool `10.66.0.0/30`, SOCKS `10.66.0.1:1080`.
- Клиент запущен через host-network compose: listen `127.0.0.1:443`, server
  `REDACTED:443`, TUN `fpsc0`, `auto_configure=true`.
- Добавлен runtime-only persistent service `echo-carrier-hold` с
  `fps_carrier hold-wss --connect 127.0.0.1:443 --preset echo_websocket_org`.
- Во время проверки найден bug в `hold-wss`: публичный `echo.websocket.org`
  отправляет начальное greeting-сообщение до echo payload, а helper ожидал
  первый recv равным payload. Исправлено: helper игнорирует non-matching
  pre-echo messages в пределах `--io-timeout` и логирует
  `event=hold_wss_ignored_messages`.
- Integration harness расширен greeting-сообщением для fake echo origin;
  покрыты direct carrier test и Zero-RTT hold-wss chain.
- `docs/docker.md` обновлен: Docker-run `hold-wss` к host-loopback FPS client
  требует `--network host`, а public echo greeting теперь описан.
- `dev/UX_FLOW_REVIEW.md` полностью переписан под clean echo flow, проверки,
  найденные friction points и текущие runtime commands.

Проверка:

- `python3 -m py_compile tools/fps_carrier.py tests/integration/carrier_tool.py tests/integration/wss_zero_rtt_hold.py tests/integration/fps_https_harness.py`
- `python3 tests/integration/carrier_tool.py --carrier /workspaces/tools/fps_carrier.py`
- `python3 tests/integration/wss_zero_rtt_hold.py --fps-client ./build/fps_client --fps-server ./build/fps_server --carrier /workspaces/tools/fps_carrier.py`
- `docker build -f Dockerfile.alpine -t fps:alpine .`
- `docker run --rm -v /tmp/fps-echo-flow/server/config:/etc/fps:ro fps:alpine fps_server --check-config --config /etc/fps/server.json`
- `docker run --rm -v /tmp/fps-echo-flow/client/config:/etc/fps:ro fps:alpine fps_client --check-config --config /etc/fps/client.json`
- `docker run --rm --network host fps:alpine fps_carrier hold-wss --connect echo.websocket.org:443 --preset echo_websocket_org --rps 2 --messages 1 --no-reconnect`
- `docker run --rm --network host fps:alpine fps_carrier hold-wss --connect 127.0.0.1:443 --preset echo_websocket_org --rps 2 --messages 3 --no-reconnect`
- `cd /tmp/fps-echo-flow/client && docker compose run --rm --no-deps fps-client status`
- `ssh fpshop 'cd /root/fps-echo-flow/server && docker compose run --rm --no-deps fps-server status'`
- `ping -c 2 -W 2 10.66.0.1`
- SOCKS5 CONNECT through `10.66.0.1:1080` to `echo.websocket.org:443` plus TLS
  SNI handshake completed with TLS 1.3.

Решения:

- Оставить final runtime services running для пользовательских follow-up tests.
- Использовать `fps:alpine` как runtime в повторном flow, чтобы подтвердить
  исправление Alpine UUID/auth path.
- Исправить `hold-wss` сразу, потому documented echo preset был блокирующим для
  требуемой базовой перепроверки.

Незакрытые вопросы:

- Нужен ли официальный persistent `echo-carrier-hold` compose snippet в
  `examples/docker/`.
- Нужен ли deployment bundle/init helper, который генерирует server/client JSON
  и compose files без ручной сборки runtime directory.
- Нужно ли добавлять optional network-dependent smoke против публичного
  `wss://echo.websocket.org` отдельно от локальных deterministic integration
  tests.

### Polish Docker operator flow

Цель:

- Закрыть оставшиеся UX friction points перед повторным user-flow тестом:
  status inspection из Docker, server public `:443` example, client-host route
  defaults и ownership подсказки для generated profiles.

Сделано:

- `docker/fps-entrypoint.sh` получил role-aware aliases `check-config` и
  `status`, которые используют `FPS_ROLE`/`FPS_CONFIG`; явные команды вроде
  `fps_server --help` продолжают bypass через `exec "$@"`.
- Docker compose examples с `ops.status_socket` теперь шарят named `/run/fps`
  volume, чтобы one-shot `docker compose run --rm --no-deps ... status` видел
  UNIX socket daemon container.
- Server compose публикует `${FPS_PUBLISHED_PORT:-8443}:8443/tcp`; docs
  показывают `FPS_PUBLISHED_PORT=443`.
- Client host-network compose больше не задаёт `FPS_TUN_ROUTES` по умолчанию:
  lease auto-config создаёт connected route, остальные routes остаются явным
  operator policy.
- Docs обновлены с host-redirection / `docker run --user "$(id -u):$(id -g)"`
  вариантами, чтобы generated profiles на bind mounts не неожиданно становились
  root-owned.

Проверка:

- `bash -n tools/*.sh docker/*.sh`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `FPS_PUBLISHED_PORT=443 docker compose -f examples/docker/server/compose.yml config`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `git diff --check`

### Raw UUID CLI and close diagnostics

Цель:

- Упростить UUID helper: `fps_client --generate-client-uuid` должен печатать
  только canonical raw UUID, без `--raw` и без `client_uuid=<uuid>` wrapper.
- Закрыть UX gap по диагностике закрытий carrier/session: оператору нужен
  metadata-only close reason в логах и status JSON.

Сделано:

- Удалён `--raw` из CLI parser/help/tests/smoke; UUID parser больше не содержит
  специальной ветки для `client_uuid=<uuid>`.
- `TcpBridgeSessionStats` получил описанный `close` metadata block: reason,
  optional direction/component/stage и error name без payload/secret bytes.
- Relay пишет `event=session_closed reason=... close={...}`, а status socket
  отдаёт `sessions.last_closed` и bounded `sessions.recent_closed`.
- Ранние target resolve/connect failures также попадают в recent close buffer,
  чтобы status объяснял закрытие даже до старта bridge-сессии.
- Документация обновлена: raw UUID-only UX, close diagnostics и смысл
  `write_queue_full` для TUN packets vs authenticated cover stream.

Проверка:

- `cmake --build build -j 2` (промежуточно после C++ изменений)
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `git diff --check`

## 2026-05-15

### Add echo WSS carrier hold UX

Цель:

- Закрыть следующий UX gap из `dev/UX_FLOW_REVIEW.md`: оператору нужен
  persistent carrier helper для реального echo-style origin, без REDACTED-specific
  JSON-RPC логики.
- Добавить script-friendly UUID output, optional status socket в generated
  profiles и приглушить повторяющийся пред-carrier/log-noise.

Сделано:

- Сначала добавлены regression tests: direct `fps_carrier hold-wss` против
  локального fake TLS WSS echo origin, Zero-RTT chain через
  `fps_client -> fps_server -> fake echo`, CLI проверки UUID helper,
  `--client-status-socket` и profile output.
- `fps_carrier` получил subcommand `hold-wss`; default preset
  `echo_websocket_org` использует Host/SNI/path для `wss://echo.websocket.org`,
  а TCP endpoint задаётся через `--connect`.
- Default hold rate: `--rps 0.1`; helper валидирует deterministic echo payloads,
  поддерживает `--messages`, `--duration`, `--ready-file`, reconnect/backoff и
  metadata-only logs.
- Script-friendly UUID helper был добавлен в этой итерации и затем упрощён в
  следующей до raw-only `fps_client --generate-client-uuid`.
- `fps_server --generate-client-profile --client-status-socket PATH` добавляет
  `ops.status_socket` в generated client JSON/URI.
- Добавлен count-based `fps::log::RepetitionLimiter`; relay rate-limit'ит
  повторяющиеся `no_carrier_session`, `non_ipv4_tun_destination`,
  `write_queue_full` и `tls_parse_error invalid_header`, сохраняя точные
  counters в status JSON.
- Обновлены `docs/docker.md`, `docs/client-ux.md`, `docs/specification.md`,
  `docs/testing.md`, `README.md`, `dev/UX_FLOW_REVIEW.md`.

Промежуточная проверка:

- Initial TDD check:
  `python3 tests/integration/carrier_tool.py --carrier /workspaces/tools/fps_carrier.py`
  failed with argparse `invalid choice: 'hold-wss'`.
- Initial C++ TDD check:
  `cmake --build build -j 2` failed on missing
  `fps/log/rate_limiter.hpp` and missing `generate_client_uuid_raw`.
- `python3 tools/fps_carrier.py hold-wss --help`
- `python3 tests/integration/carrier_tool.py --carrier /workspaces/tools/fps_carrier.py`
- `python3 tests/integration/wss_zero_rtt_hold.py --fps-client ./build/fps_client --fps-server ./build/fps_server --carrier /workspaces/tools/fps_carrier.py`
- `./build/fps_unit_tests --run_test=logging/repetition_limiter_reports_suppressed_events --catch_system_errors=no`
- `./build/fps_unit_tests --run_test=tcp_relay_app/cli_parses_check_config_and_key_tool_commands --catch_system_errors=no`
- `./build/fps_unit_tests --run_test=tcp_relay_app/server_cli_generates_valid_client_profile --catch_system_errors=no`
- `ctest --test-dir build -R 'fps_cli_streams|fps_carrier_tool|fps_wss_zero_rtt_hold' --output-on-failure`

Финальная проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `cmake --build build -j 2`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `git diff --check`

Commit:

- This logical commit: `Improve echo carrier UX and scriptable profiles`.

### Fix authenticated cover chunking and Alpine UUID smoke

Цель:

- Закрыть beta blocker из `dev/UX_FLOW_REVIEW.md`: browser carrier не должен
  закрываться, когда origin отдаёт крупный/coalesced server-to-client TLS byte
  stream после Zero-RTT auth.
- Исправить Alpine runtime failure для UUID-derived client identity и усилить
  Docker smoke, чтобы такой регресс ловился до публикации образа.

Сделано:

- Сначала добавлен regression unit test
  `server_role_chunks_large_authenticated_origin_reads`; на стабильном коде он
  упал из-за закрытия session на большом authenticated origin read, подтвердив
  гипотезу.
- Post-auth cover bytes в `TcpBridgeSession` теперь режутся на безопасные
  chunks и кодируются в несколько FPS envelope TLS records через scratch
  outbound pipeline. Реальный outbound pipeline sequence обновляется только
  после успешного encode и write-queue preflight.
- Добавлен unit test на preflight: при заведомо малой write queue large read
  закрывает session без partial enqueue.
- `FpsEnvelopePipeline::encode_tls_record` теперь сохраняет stage/cause
  encode failure, а relay logs пишут `event=envelope_encode_error stage=...`.
- `hkdf_sha256` использует explicit SHA-256 zero salt при empty salt, что
  сохраняет RFC HKDF семантику и чинит OpenSSL 3.5/Alpine UUID derivation.
- Добавлен local HTTPS Zero-RTT integration test с большим response body.
- Docker smoke теперь генерирует client UUID/server keypair и запускает
  `fps_server --check-config` с generated allowlist.

Проверка:

- Initial TDD check before implementation:
  `./build/fps_unit_tests --run_test=tcp_bridge_session/server_role_chunks_large_authenticated_origin_reads --catch_system_errors=no`
  failed on stable code with `critical check !fixture.closed_stats.has_value()`.
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=tcp_bridge_session --catch_system_errors=no`
- `./build/fps_unit_tests --run_test=fps_envelope_pipeline/encode_surfaces_envelope_and_record_errors --catch_system_errors=no`
- `./build/fps_unit_tests --run_test=identity/derives_deterministic_x25519_keypair_from_uuid --catch_system_errors=no`
- `ctest --test-dir build -R 'fps_https_zero_rtt_large_response' --output-on-failure`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `FPS_DOCKERFILE=Dockerfile.alpine FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:alpine tools/run_quality_checks.sh --docker`
- `tools/docker_tun_iperf_sim.py --image fps:alpine --duration 10 --bandwidth 5M --length 1200`
  прошёл с примерно 5.0 Mbit/s UDP и 0% loss.
- `git diff --check`

Commit:

- This logical commit: `Fix authenticated cover chunking and Alpine UUID smoke`.

### Follow up REDACTED browser carrier diagnostics

Цель:

- Разобрать пользовательский прогон после реального SOCKS/browser теста,
  проверить состояние сервисов и логи, отдельно диагностировать разрушение
  browser/WebSocket carrier-сессий со временем.

Сделано:

- Проверены запущенные сервисы без перезапуска и без изменений локальных
  DNS/routes/firewall/NAT/iptables/nftables/resolvectl.
- Сняты status counters из уже работающих контейнеров:
  локальный клиент держит `carriers_current=1`, сервер держит
  `carriers_current=1`; суммарно зарегистрировано 36 authenticated carriers.
- Сверены логи клиента, сервера и SOCKS sidecar после пользовательского теста.
  SOCKS-трафик был подтвержден логами Dante, а direct browser path подтвердился
  authenticated carrier-сессиями и последующими removals.
- Запущен отдельный диагностический REDACTED WebSocket probe через FPS-клиент:
  `127.0.0.1:443` TCP, TLS SNI/Host `www.REDACTED.com`,
  `/ws/REDACTED`, subscription `REDACTED`,
  `public/set_heartbeat interval=30`, WebSocket pings и ответы
  `public/test` на REDACTED `test_request`.
- Probe прошел 720.388 секунд без обрыва, получил 1,936 REDACTED messages,
  23 heartbeat/test-request cycles и 11 pong responses.
- По старому `REDACTED-carrier-hold` подтверждено: helper закрывался почти ровно
  через 600 секунд и переподключался. Это похоже на app-level REDACTED keepalive
  policy, а не на транспортный сбой FPS.
- По browser carrier collapse выделен вероятный FPS issue: server-side
  authenticated outbound path оборачивает весь read buffer в один FPS envelope и
  один outer TLS record. При дефолтном `read_buffer_size=64KiB` это может
  превышать outer TLS payload budget `16KiB + 2048` на coalesced/large
  server-to-client browser traffic; при этом лог сейчас пишет общий
  `encrypt_failed`, скрывая реальную причину encode failure.
- `dev/UX_FLOW_REVIEW.md` дополнен блоками пользовательской валидации,
  диагностики carrier stability и явным `Issues To Be Resolved`.

Проверка:

- `docker exec fps-REDACTED-client-fps-client-1 fps_client --status --config /etc/fps/client.json`
- `ssh fpshop 'docker exec fps-REDACTED-fps-server-1 fps_server --status --config /etc/fps/server.json'`
- `docker logs fps-REDACTED-client-fps-client-1 2>&1 | rg -n "envelope_error|tls_parse_error|carrier_removed|carrier_registered|tun_write_rejected|tun_read_session_error" | tail -n 80`
- `ssh fpshop 'docker logs fps-REDACTED-fps-server-1 2>&1 | grep -E "envelope_error|tls_parse_error|carrier_removed|carrier_registered|tun_write_rejected|tun_read_session_error" | tail -n 100'`
- `/tmp/fps-REDACTED-client/REDACTED_heartbeat_probe.py` через локальный FPS
  клиент завершился `event=completed`.
- `rg -n "encrypt_failed|encode_tls_record|outbound_envelope_pipeline|FpsEnvelopeError" src include tests`

Решения:

- Runtime-сервисы не менять и не перезапускать в этом инкременте, чтобы не
  подменить условия пользовательского follow-up теста.
- Зафиксировать browser carrier collapse как issue, требующий отдельного
  воспроизводимого теста и исправления fragmentation/error reporting, а не как
  только UX-недочет.
- Зафиксировать REDACTED hold helper keepalive как операторский/tooling issue:
  helper должен поддерживать `public/set_heartbeat` и ответы `public/test`.

Незакрытые вопросы:

- Нужно реализовать fragmentation/chunking authenticated outbound TLS bytes до
  outer TLS record limit и добавить regression test на large/coalesced origin
  reads.
- Нужно протащить точную причину encode failure в логи/status вместо общего
  `encrypt_failed`.
- Нужно решить, обновлять ли runtime-only `REDACTED-carrier-hold` прямо в текущей
  среде или оставить его как воспроизводящий 10-минутный REDACTED keepalive
  symptom.

### Review REDACTED deployment UX flow on real VM

Цель:

- Пройти пользовательский flow разворачивания FPS-сервера на `fpshop` и
  локального FPS-клиента для origin `www.REDACTED.com:443`, включить SOCKS
  sidecar и зафиксировать UX-вопросы/предложения.

Сделано:

- Пересобран `fps:alpine` и передан на `fpshop`; при проверке обнаружен
  runtime blocker: Alpine image не проходит `fps_server --check-config` для
  Zero-RTT UUID auth (`failed to derive client private key from UUID`).
- Для продолжения flow пересобран и передан на VM Ubuntu-based `fps:local`.
- Runtime-конфиги с новым server keypair и client UUID созданы вне репозитория:
  `/root/fps-REDACTED/` на VM и `/tmp/fps-REDACTED-client/` локально.
- Сервер запущен на `fpshop` с public bind `0.0.0.0:443`, origin
  `www.REDACTED.com:443`, TUN `fpss0`, lease pool `10.66.0.0/30` и SOCKS5
  sidecar `10.66.0.1:1080`.
- Локальный клиент запущен через host-network compose на `127.0.0.1:443` с TUN
  `fpsc0`, без ручных DNS/route/firewall/NAT/iptables/nftables/resolvectl
  изменений.
- Добавлен runtime-only compose service `REDACTED-carrier-hold`, удерживающий
  REDACTED WebSocket subscription `REDACTED.REDACTED` открытой
  через FPS, чтобы carrier оставался доступен для пользовательских тестов.
- Создан `dev/UX_FLOW_REVIEW.md` с замечаниями, вопросами и предложениями по UX.

Проверка:

- `docker build -f Dockerfile.alpine -t fps:alpine .`
- `docker save fps:alpine | gzip -1 | ssh fpshop 'gunzip | docker load'`
- `docker run --rm -v <tmp>:/etc/fps:ro fps:alpine fps_server --check-config --config /etc/fps/server.json`
  воспроизвел Alpine blocker.
- `docker build -t fps:local .`
- `docker save fps:local | gzip -1 | ssh fpshop 'gunzip | docker load'`
- `docker run --rm -v /tmp/fps-REDACTED-client/config:/etc/fps:ro fps:local fps_client --check-config --config /etc/fps/client.json`
- `ssh fpshop 'cd /root/fps-REDACTED && docker run --rm -v "$PWD/config:/etc/fps:ro" fps:local fps_server --check-config --config /etc/fps/server.json'`
- `ssh fpshop 'cd /root/fps-REDACTED && docker compose -f compose.server.yml --profile socks up -d'`
- `cd /tmp/fps-REDACTED-client && docker compose -f compose.client.yml up -d`
- Python stdlib WebSocket smoke через `127.0.0.1:443` с TLS SNI
  `www.REDACTED.com` получил успешный REDACTED `public/subscribe` response.
- Status JSON на клиенте и сервере показал `carriers_current=1` после запуска
  hold service.
- SOCKS5 CONNECT через `10.66.0.1:1080` до `www.REDACTED.com:443` с TLS SNI
  вернул `HTTP/1.1 200 OK`.

Решения:

- Отступить от первоначального `fps:alpine` runtime и использовать `fps:local`,
  чтобы не блокировать пользовательский end-to-end flow.
- Не трогать локальные DNS/routes/firewall вручную; принять только connected
  route `10.66.0.0/30`, созданный ядром при применении lease на `fpsc0`.
- Оставить сервер, SOCKS, клиент и carrier hold service запущенными для
  пользовательских тестов.

Незакрытые вопросы:

- Нужно исправить или заблокировать Alpine runtime для Zero-RTT UUID auth перед
  рекомендацией Alpine как production image.
- Нужен официальный real-origin carrier hold/smoke helper, чтобы не собирать
  такой скрипт вручную при каждом операторском прогоне.

### Default local CMake builds to Release O2

Цель:

- Сделать production-like сборку дефолтом для CMake и явно зафиксировать
  Release optimization level `-O2`.

Сделано:

- Корневой `CMakeLists.txt` теперь ставит `CMAKE_BUILD_TYPE=Release` для
  single-config generators, если пользователь не передал build type явно.
- Для GNU/Clang Release builds выставлены `CMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"`.
- `README.md` и `docs/testing.md` обновлены: quick build больше не требует
  `-DCMAKE_BUILD_TYPE=Debug`, а debug/sanitizer/coverage сборки остаются
  явными override-сценариями.

Решения:

- CI/quality/debug/sanitizer/fuzz wrappers продолжают явно передавать нужный
  build type и flags; новый дефолт применяется только там, где build type не
  задан пользователем.

Проверка:

- `rm -rf build && cmake -S . -B build && cmake --build build -j 2`
- `cmake -LA -N build | rg 'CMAKE_BUILD_TYPE|CMAKE_CXX_FLAGS_RELEASE'`
  подтвердил `CMAKE_BUILD_TYPE=Release` и `CMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG`.
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `tools/run_quality_checks.sh --all`
- `FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `FPS_DOCKER_COMPILER=clang FPS_DOCKER_IMAGE=fps:local-clang tools/run_quality_checks.sh --docker`
- `FPS_DOCKERFILE=Dockerfile.alpine FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:alpine tools/run_quality_checks.sh --docker`
- `tools/docker_tun_iperf_sim.py --build --duration 10 --bandwidth 5M --length 1200`
  прошёл с примерно 5.0 Mbit/s UDP и 0% loss.
- `tools/docker_socks_smoke.py --build`
- `tools/docker_multi_client_sim.py --build`
- `rm -rf cmake-build-pcap && cmake -S . -B cmake-build-pcap -DFPS_ENABLE_PCAP_TESTS=ON && cmake --build cmake-build-pcap -j 2 && ctest --test-dir cmake-build-pcap -L pcap --output-on-failure`
- `sudo -n rm -rf cmake-build-tun && cmake -S . -B cmake-build-tun -DFPS_ENABLE_TUN_TESTS=ON && cmake --build cmake-build-tun -j 2 && sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`

Commit:

- This logical commit: `Default CMake builds to Release O2`.

### Move CI local jobs into Dockerfile ci stage

Цель:

- Убрать дублирование Boost/OpenSSL/Python/LLVM package lists из GitHub Actions
  и использовать репозиторный `Dockerfile` как источник CI build environment.

Сделано:

- Основной `Dockerfile` разделен на `build-deps`, `ci`, `builder` и `runtime`
  stages; product runtime stage не изменяет состав зависимостей из-за CI.
- Добавлен `docker/ci-local.sh`: syntax checks, Debug configure/build и
  ordinary non-sudo CTest внутри CI container.
- Workflow `CI` теперь собирает `docker build --target ci` для GCC/clang и
  запускает tests через `docker run`.
- Workflow `Quality` теперь запускает `tools/run_quality_checks.sh --all`
  внутри того же `ci` stage вместо ручной установки пакетов на runner.
- CI/Quality Docker build steps используют `docker/setup-buildx-action` и
  `docker buildx build --load`, чтобы target build не проходил через unrelated
  product runtime stages.
- `tools/run_quality_checks.sh --docker` автоматически использует
  `docker buildx build --load`, если buildx доступен (`FPS_DOCKER_BUILDKIT=0`
  оставлен как escape hatch).
- `fps_docker_artifacts` и `docs/testing.md` обновлены под новый CI contract.

Решения:

- Не переносить root/TUN, pcap и long Docker soak в CI stage; они остаются
  opt-in до появления выделенного privileged runner.
- Не использовать Alpine для local/quality CI tests в этом инкременте:
  Alpine покрывается product Docker smoke, а GCC/clang beta matrix остается на
  Ubuntu 24.04.
- `ci` stage расположен до product builder/runtime stages, чтобы target build
  мог остановиться на CI image, а default `docker build Dockerfile` всё равно
  заканчивался product runtime image; BuildKit в CI пропускает unrelated stages.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`
- `docker build --target ci --build-arg FPS_COMPILER=gcc -t fps:ci-local-gcc .`
- `docker run --rm -e FPS_JOBS=2 fps:ci-local-gcc`
- `docker build --target ci --build-arg FPS_COMPILER=clang -t fps:ci-local-clang .`
- `docker run --rm -e FPS_JOBS=2 fps:ci-local-clang`
- `docker run --rm -e FPS_FUZZ_RUNS=1 -e FPS_JOBS=2 fps:ci-local-clang tools/run_quality_checks.sh --python`
- `FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`

Commit:

- This logical commit: `Run CI checks inside Dockerfile stage`

### Add Alpine production Dockerfile

Цель:

- Проверить, годится ли Alpine Linux для боевого Docker runtime, и добавить
  отдельный `Dockerfile.alpine`, если он дает заметно меньший полнофункциональный
  образ без сборки runtime-зависимостей из исходников.

Сделано:

- Проверены Alpine 3.20/3.21/3.22 package repositories для Boost, OpenSSL,
  Dante, iproute2, iperf3, Python, tini и websockets.
- Собран экспериментальный Alpine 3.22 image: C++ build проходит через
  `boost-dev`, `build-base`, `cmake`, `linux-headers`, `openssl-dev`.
- Runtime использует apk-пакеты для Boost/OpenSSL/Dante/iproute2/iperf3/tini,
  Bash для существующих entrypoints и pinned `websockets==12.0` musllinux wheel
  через temporary `py3-pip`.
- Добавлен `Dockerfile.alpine`; Dante `sockd` получает symlinks `danted`, чтобы
  сохранить существующий Docker smoke/SOCKS entrypoint contract.
- `tools/run_quality_checks.sh --docker` получил `FPS_DOCKERFILE`.
- Compose examples теперь принимают `FPS_DOCKERFILE=Dockerfile.alpine` без
  ручного редактирования YAML.
- CI Docker smoke matrix расширена Alpine GCC образом.
- Docker docs/testing/beta-readiness/roadmap обновлены.

Решения:

- Alpine image выбран как production-oriented smaller image: экспериментальный
  размер `fps:alpine-experiment` был около 92.5MB против Ubuntu `fps:local`
  около 221MB.
- Alpine Dockerfile пока поддерживает только GCC builder path; clang остается
  покрытым Ubuntu CI/reference Dockerfile.
- `py3-websockets` из Alpine 3.22 не используется, потому версия 15 меняет
  server handler API; pinned `websockets==12.0` совместим с текущим
  `fps_carrier` и ставится готовым musllinux wheel.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local tools/run_quality_checks.sh --docker`
- `FPS_DOCKERFILE=Dockerfile.alpine FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:alpine tools/run_quality_checks.sh --docker`
- Alpine `fps_carrier` WSS roundtrip inside a temporary Docker network:
  origin/client completed `DONE frames=4`.
- `docker images` comparison after the smoke builds: `fps:alpine` about 92.5MB,
  `fps:local` about 221MB.

Commit:

- This logical commit: `Add Alpine production Dockerfile`

## 2026-05-14

### Add beta CI matrix

Цель:

- Начать beta release engineering с GitHub Actions матрицы
  `ubuntu-24.04 x gcc/clang`, сохранив Docker как главный product artifact.

Сделано:

- Добавлен PR/push/manual workflow `CI`: локальная non-sudo сборка и CTest для
  GCC/clang, плюс Docker build/runtime smoke для обоих компиляторов.
- Добавлен scheduled/manual workflow `Quality`, запускающий
  `tools/run_quality_checks.sh --all` с clang-20, sanitizers, Valgrind,
  llvm-cov и bounded libFuzzer smoke.
- `Dockerfile` получил `FPS_COMPILER=gcc|clang`; clang-20 ставится только в
  builder stage, runtime image остается тем же продуктовым образом.
- `tools/run_quality_checks.sh --docker` принимает `FPS_DOCKER_COMPILER` и
  передает его в Docker build.
- `fps_docker_artifacts` проверяет compiler-arg contract Dockerfile.
- Docker clang smoke выявил недостающие runtime Boost shared libraries;
  runtime stage теперь явно включает полный набор Boost libs, нужный обоим
  compiler outputs.
- `docs/testing.md`, `docs/beta-readiness.md` и `dev/ROADMAP.md` обновлены под
  новый CI baseline.

Решения:

- PR CI остается быстрым и непривилегированным; root/TUN, pcap и long Docker
  soak остаются opt-in вне этого инкремента.
- Heavy checks живут в scheduled/manual workflow, чтобы не превращать каждый PR
  в долгий и хрупкий прогон.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `FPS_DOCKER_SUDO=1 FPS_DOCKER_COMPILER=clang tools/run_quality_checks.sh --docker`
- `FPS_DOCKER_SUDO=1 FPS_DOCKER_COMPILER=gcc tools/run_quality_checks.sh --docker`
- `git diff --check`

Commit:

- This logical commit: `Add beta CI matrix`

### Beta readiness review refresh

Цель:

- Актуализировать beta-readiness и self-review после refactor/cleanup
  инкрементов без изменения runtime/protocol/config behavior.

Сделано:

- `docs/beta-readiness.md` обновлен под текущее состояние: private beta
  candidate, текущая schema-only config/log surface, реализованные `fps://v1`,
  safe profile output, status socket, Docker SOCKS sidecar, strict lease routing
  и quality baseline.
- Beta gate сжат до конкретных blockers: CI/release matrix, long Docker/TUN
  soak, independent Zero-RTT/protocol review, UUID/key rotation workflow и
  public onboarding examples.
- `dev/REVIEW.md` обновлен свежими findings по `tcp_relay_app.cpp` split,
  Boost.Describe structured logs, schema-only parser/tooling и оставшимся
  beta/code hotspots.
- `dev/ROADMAP.md` уточняет cleanup completed и переименовывает CI/platform
  phase без изменения deferred work.

Решения:

- Public docs остаются в `docs/` на английском; developer review/worklog
  остаются в `dev/`.
- Исторический `dev/WORKLOG.md` сохраняется как journal, но две старые строки
  с удаленными pre-production names переформулированы, чтобы простая grep
  проверка не выглядела как current contract leak.
- CI, soak, rotation и release signing не реализовывались в этом docs-only
  инкременте.

Проверка:

- Stale-token `rg` scan from the review plan across `docs`, `dev` and
  `README.md`.
- `cmake --build build -j 2`
- `ctest --test-dir build -L local --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`

Commit:

- `Refresh beta readiness review`.

### Remove pre-production compatibility checks

Цель:

- Убрать legacy compatibility-layer проверки и адаптеры, потому что у FPS еще
  нет публичного legacy config/log контракта, который нужно поддерживать.

Сделано:

- Удален explicit reject-list для старых Zero-RTT key-file/client-key/
  public-key allowlist fields из config parser.
- Удалены миграционные ошибки для `network.read_buffer` и `iff.enabled`; parser
  валидирует только текущие production поля (`network.read_buffer_size`,
  `security.zero_rtt.*`, `tun.*`).
- Docker iperf stats parser теперь принимает только текущий structured
  `event=session_stats stats={...}` формат, без fallback на старые expanded
  `c2s_*`/`s2c_*` key=value counters.
- Тесты и docs очищены от legacy-specific rejection cases и IFF log omission
  checks; CLI tests оставляют только generic unknown-option behavior.

Решения:

- TLS `legacy_version` fields не трогались: это термин TLS record format, а не
  FPS compatibility layer.
- Race-safe trial-decrypt fallback для Zero-RTT confirmation не трогался: это
  текущая корректность протокола, а не поддержка старого wire format.

Проверка:

- `cmake --build build -j 2`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `ctest --test-dir build -R fps_unit_tests --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`

Commit:

- This logical commit: `Remove pre-production compatibility checks`

### Split relay config parsing from runtime

Цель:

- Уменьшить размер и когнитивную нагрузку `src/net/tcp_relay_app.cpp` после
  Boost.Program_options/Describe инкрементов.

Сделано:

- Вынесен новый compilation unit `src/net/tcp_relay_config.cpp` для endpoint
  parsing, Boost.JSON config helpers, shaper config parsing и Zero-RTT config
  parsing.
- `tcp_relay_app.cpp` уменьшен примерно с 3406 до 2350 строк; runtime file
  теперь меньше смешивает config validation и live relay/session orchestration.
- `security.zero_rtt.upgrade_direction` парсится через Describe enum metadata
  (`enum_from_name<Direction>`).
- `endpoint_parse_error_message(...)` переведен на Describe-style machine enum
  names через `enum_name_or(...)`, без отдельного switch.

Решения:

- Это первый safe split. CLI helpers/profile/lease/status commands пока остаются
  в `tcp_relay_app.cpp`; их лучше выносить отдельным следующим инкрементом,
  чтобы не смешивать большой mechanical move с изменением CLI поведения.
- Program_options уже используется в `parse_tcp_relay_cli(...)`, поэтому этот
  инкремент не меняет CLI parser contract.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build -R fps_unit_tests --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `git diff --check`

Commit:

- This logical commit: `Split relay config parsing from runtime`

### Boost.Describe structured log fields

Цель:

- Уменьшить ручное перечисление полей в operational logs, прежде всего
  `event=session_stats` и shaper/TUN status traces.

Сделано:

- Добавлен `fps/log/describe.hpp`: Boost.Describe metadata превращается в
  Boost.JSON object/string через `described_to_json(...)` и
  `fps::log::as_json(...)`.
- `TcpBridgeSessionStats`, `TcpBridgeDirectionStats`,
  `TcpBridgeShaperEvent`, `TunLinkConfigureStatus` и
  `TunLeaseConfigureStatus` описаны через `BOOST_DESCRIBE_STRUCT`.
- Relay runtime logs теперь пишут compact structured tails:
  `stats={...}`, `detail={...}`, `status={...}` вместо длинных ручных цепочек
  `<< " field=" << value`.
- Docker iperf/multi-client parser понимает новый JSON stats tail и сохраняет
  fallback для старых key=value session counters.

Решения:

- Общий log sink пока остается text-field формата `ts=... level=...`, но
  structured tail является валидным JSON. Полный JSON-per-line logger лучше
  делать отдельным инкрементом, чтобы не ломать текущие grep-friendly tests.
- В Describe-сериализацию добавлена поддержка enums, nested described structs,
  arithmetic values, strings, optional, arrays/vectors и chrono durations
  (duration serializes as milliseconds count).
- В structured logs попадают только counters/state metadata; UUID, keys,
  ClientID, raw TLS/TUN/envelope payloads не сериализуются.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build -R fps_unit_tests --output-on-failure`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `ctest --test-dir build -L local --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`

Commit:

- This logical commit: `Use Boost.Describe for structured logs`

### Boost.Endian wire helpers

Цель:

- Убрать повторяющиеся hand-rolled big-endian integer helpers из protocol,
  control и TUN framing code.

Сделано:

- Добавлен `fps/core/wire.hpp` с `append_be(...)` и `read_be<T>(...)` поверх
  Boost.Endian.
- TLS record layer/parser, covert codec, FPS envelope codec, Zero-RTT upgrade,
  TUN fragmentation/session manager и TUN lease/control parsing переведены на
  общий helper.
- Удалены локальные `append_u16_be`, `append_u32_be`, `append_u64_be`,
  `read_u16_be`, `read_u32_be`, `read_u64_be` из C++ production code.
- Добавлены unit-тесты `wire_helpers`; fragment payload helpers в session
  manager tests тоже используют общий helper.

Ревизия:

- Boost.Endian оказался хорошим fit: формат wire не меняется, а размер
  дублированного byte-shift code заметно уменьшился.
- `Boost.UUID` пока не внедрялся: текущая UUID логика маленькая, но связана с
  crypto-random generation, strict v4 validation и deterministic X25519
  derivation. Менять ее лучше отдельным targeted step, если появится реальная
  боль.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build -R fps_unit_tests --output-on-failure`
- `git diff --check`

Commit:

- This logical commit: `Use Boost.Endian for wire integers`

### Boost.Program_options CLI parsing

Цель:

- Уменьшить ручной argv parsing в Linux runtime CLI без изменения внешнего
  поведения команд.

Сделано:

- `parse_tcp_relay_cli(...)` теперь использует Boost.Program_options для
  tokenization, known-option validation, value extraction and missing-argument
  handling.
- Сохранена проектная семантика команд: mutual exclusion, server/client-only
  commands, profile output rules, status socket override, lease command rules и
  non-secret diagnostics остаются в FPS code.
- `--log-level`, `--listen`, target endpoint и `--read-buffer` работают как CLI
  overrides поверх JSON config.
- Добавлен Boost `program_options` component в CMake и Docker builder/runtime
  dependencies.

Ревизия:

- Program_options полезен как parser/tokenizer, но не должен владеть всей
  бизнес-семантикой CLI: validation around profile/status/lease commands всё
  еще лучше держать рядом с typed FPS config.
- Следующие Boost candidates: `Boost.Endian` для общих big-endian read/write
  helpers; опционально `Boost.UUID` только если получится сохранить
  crypto-random UUID generation and strict v4 validation без ухудшения UX.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build -R 'fps_unit_tests|fps_cli_streams' --output-on-failure`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `ctest --test-dir build -L local --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`

Commit:

- This logical commit: `Use Boost.Program_options for relay CLI`

### Boost.Describe enum cleanup

Цель:

- Уменьшить boilerplate вокруг enum conversions перед более крупной CLI
  миграцией на Boost.Program_options.

Сделано:

- Добавлен `fps/core/enum.hpp` с generic helpers поверх Boost.Describe:
  `enum_name`, `enum_name_or`, `enum_from_name`, case-insensitive lookup,
  `enum_from_underlying`, `enum_index` и `enum_count`.
- Описаны core/net/log enums через `BOOST_DESCRIBE_ENUM`.
- Убраны повторяющиеся `enum -> string` switches для runtime logs/status в
  `tcp_relay_app.cpp` и `logging.cpp`.
- `CovertCodec` и `FpsEnvelopeCodec` валидируют `FrameType` по описанному
  набору enum values, а не по диапазону.
- Добавлены unit-тесты для enum helpers.

Ревизия:

- Boost.Describe хорошо подходит для log/status stringification и bounded
  numeric decode.
- Boost.Program_options стоит внедрять отдельным логическим коммитом:
  текущий parser велик, но содержит много семантических правил profile/status/
  lease команд, которые нужно сохранить без изменения stdout/stderr UX.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build -R fps_unit_tests --output-on-failure`
- `git diff --check`

Commit:

- This logical commit: `Use Boost.Describe for enum helpers`

### Runtime status socket

Цель:

- Добавить минимальный beta health surface для операторов без превращения его в
  management API.

Сделано:

- Добавлен optional `ops.status_socket`: daemon поднимает локальный UNIX socket,
  на каждый connect отдает один JSON snapshot и закрывает соединение.
- Добавлены `fps_client --status --config client.json` и
  `fps_server --status --config server.json`, плюс `--status-socket PATH` для
  ad-hoc override.
- Snapshot включает role, pid, uptime, listen/target, session/carrier counters,
  TUN packet/drop counters и shaper counters; UUID, ключи, ClientID и payload
  bytes не выводятся.
- Docker runtime создает `/run/fps`; Docker examples включают status socket.
- Добавлен local `fps_status_socket` integration smoke.
- README, spec, Docker/testing docs, beta review, self review и roadmap
  обновлены.

Проверка:

- `python3 -m py_compile tests/integration/status_socket.py tests/integration/cli_streams.py`
- `cmake --build build -j 2`
- `ctest --test-dir build -R 'fps_unit_tests|fps_cli_streams|fps_status_socket' --output-on-failure`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`

Commit:

- This logical commit: `Add runtime status socket`

### Safe client profile file output

Цель:

- Продолжить beta UX: убрать необходимость shell redirection для выдачи
  профилей и импорта `fps://v1` URI в client config.

Сделано:

- `fps_server --generate-client-profile ... --output PATH [--force]` пишет JSON
  или URI profile в файл с правами `0600`; существующий файл не перезаписывается
  без `--force`.
- Добавлен `fps_client --write-config-from-uri URI --output PATH [--force]`.
- `--print-config-from-uri` и stdout behavior для profile generation оставлены
  совместимыми.
- Unit и `fps_cli_streams` проверяют roundtrip, overwrite protection, `--force`,
  file mode `0600` и отсутствие server private key leak.
- README, spec, Docker docs, testing docs, beta/UX review и roadmap обновлены.

Проверка:

- `python3 -m py_compile tests/integration/cli_streams.py`
- `cmake --build build -j 2`
- `ctest --test-dir build -R 'fps_unit_tests|fps_cli_streams' --output-on-failure`

Commit:

- This logical commit: `Add safe client profile file output`

### Client profile URI UX

Цель:

- Завершить следующий UX step после JSON profiles: добавить VLESS-like
  single-string URI, который несет тот же client profile payload.

Сделано:

- `fps_server --generate-client-profile ... --format uri` теперь печатает
  `fps://v1/<base64url-json-profile>`.
- Добавлен `fps_client --print-config-from-uri URI`, который декодирует
  `fps://v1` обратно в JSON config на stdout.
- URI payload является тем же JSON profile, URL-safe base64 без padding; отдельной
  query-string схемы нет, чтобы не дублировать profile schema.
- CLI help, unit tests, `fps_cli_streams`, README, Docker docs, spec, testing docs,
  beta review, UX review и roadmap обновлены.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- Markdown relative link check across `*.md`
- `cmake --build build -j 2`
- `ctest --test-dir build -R 'fps_unit_tests|fps_cli_streams' --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`

Commit:

- This logical commit: `Add fps profile URI import`

### Race-safe Zero-RTT confirmation wait

Цель:

- Исправить runtime под требование из `docs/specification.md`: server
  confirmation не обязан быть первым peer-direction TLS record после
  client-side Zero-RTT upgrade.

Сделано:

- `FpsEnvelopePipelineProcessResult` получил `forward_tls_bytes` и счетчик
  `decoded_envelope_records`.
- Добавлен `FpsEnvelopePipeline::process_inbound_tls_with_trial_fallback(...)`:
  до первого успешного envelope decrypt обычные TLS records форвардятся
  byte-for-byte, bad tag не логируется как envelope error и receive sequence не
  продвигается; после первого successful envelope pipeline возвращается к
  strict envelope behavior.
- `TcpBridgeSession` в client role при ожидании confirmation использует trial
  fallback: raced origin/browser TLS records уходят клиенту как cover traffic,
  а authenticated state включается только после первого валидного envelope.
- Добавлены unit regressions для pipeline fallback и bridge scenario, где
  origin-to-client TLS record приходит раньше server confirmation.
- `docs/testing.md` обновлен с новым covered behavior.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- Markdown relative link check across `*.md`
- `cmake --build build -j 2`
- `ctest --test-dir build -R fps_unit_tests --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`

Commit:

- This logical commit: `Make Zero-RTT confirmation wait race-safe`

### Client profile generation UX

Цель:

- Продолжить beta/UX work после docs cleanup: дать администратору OpenVPN-like
  способ выдать клиенту готовый JSON profile без ручной сборки секций.

Сделано:

- Добавлен `fps_server --generate-client-profile --config PATH --client-uuid UUID
  --server-endpoint HOST:PORT`.
- Профиль включает client `network.listen`, `network.server`, Zero-RTT
  `client_uuid`, `server_public_key_base64`, profile id, codec settings,
  logging level и client-side TUN `auto_configure=true`, если server config
  включает TUN.
- Профиль не содержит `server_private_key_base64`, `allowed_client_uuids`,
  server lease pool internals или lease file path.
- Команда проверяет, что UUID есть в текущем server `allowed_client_uuids`, и
  отказывает при попытке выдать профиль неизвестному клиенту.
- CLI/help, unit tests, `fps_cli_streams`, `docs/client-ux.md`,
  `docs/specification.md`, `docs/testing.md`, `README.md`, `dev/ROADMAP.md` и
  beta review обновлены.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- Markdown relative link check across `*.md`
- `cmake --build build -j 2`
- `ctest --test-dir build -R 'fps_unit_tests|fps_cli_streams' --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `git diff --check`

Commit:

- This logical commit: `Add server-generated client profiles`

### Zero-RTT wire refactor notes

Цель:

- Перед дальнейшей beta/UX работой зафиксировать в спецификации два
  обнаруживаемых/корректностных protocol caveat-а.

Сделано:

- `docs/specification.md` помечает fixed-position 32-byte public-key prefix в
  Zero-RTT candidate как high-priority wire refactor, потому такой prefix проще
  анализировать, чем timing/size distributions.
- В спецификации добавлен requirement, что client-side server confirmation
  должен быть race-safe: trial-decrypt plausible peer records и fallback
  byte-for-byte, если record не является confirmation envelope.

Проверка:

- `git diff --check`

Commit:

- `Document Zero-RTT wire refactor risks`

### Documentation reorganization and beta UX review

Цель:

- Подготовить проект к GitHub/beta review: отделить публичную документацию от
  рабочих артефактов агентов/разработчиков.
- Перевести активные пользовательские документы и корневой README на английский.
- Удалить устаревшие research/crypto-review материалы, чьи выводы уже
  перенесены в актуальные документы.
- Зафиксировать beta-readiness и UX review по упрощению клиентского onboarding.

Сделано:

- Публичные документы перенесены в `docs/`: `specification.md`, `testing.md`,
  `docker.md`, `linux-routing.md`, `beta-readiness.md`, `client-ux.md` и
  `README.md`-индекс.
- Рабочие документы перенесены в `dev/`: `WORKLOG.md`, `ROADMAP.md`,
  `REVIEW.md`, `NETWORK_RECOVERY.md`.
- Корневой `README.md` сокращен до приветствия, quick-start и ссылок на
  `docs/`.
- `AGENTS.md` обновлен: актуальная спецификация теперь
  `docs/specification.md`, журнал работ - `dev/WORKLOG.md`, пользовательская
  документация пишется на английском в `docs/`.
- Удалены `CRYPTO_REVIEW.md` и `research/`, чтобы не поддерживать несколько
  расходящихся источников истины.
- Добавлен `docs/beta-readiness.md`: проект beta-candidate для controlled
  Linux/Docker trials, но перед публичной beta нужны profile import, status/CI,
  soak и release hardening.
- Добавлен `docs/client-ux.md`: предложен следующий UX-инкремент через
  server-generated client profiles и versioned `fps://` URI, реализованные в C++
  CLI без разрастания helper scripts.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- Markdown relative link check across `*.md`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `cmake --build build -j 2`
- `ctest --test-dir build -L local --output-on-failure`
- `git diff --check`

Commit:

- This logical commit: `Reorganize docs for beta review`

## 2026-05-13

### Sanity review, coverage cleanup and fuzzing baseline

Цель:

- Сделать паузу после Docker/SOCKS/lease-фич, провести self-review и расширить
  sanity checks перед следующими product increments.
- Закрепить strict lease source-IP enforcement интеграционным сигналом для
  multi-client topology.
- Добавить bounded libFuzzer smoke в обычный non-Docker quality set.

Сделано:

- `REVIEW.md` обновлен свежим self-review: lease enforcement, multi-client
  readiness, Docker/SOCKS, coverage gaps, fuzzing surface и stale-code cleanup.
- Удален активный compatibility хвост `network.read_buffer`; теперь config parser
  возвращает clear error и требует `network.read_buffer_size`.
- Переименован stale integration helper `write_zero_rtt_key_files` в
  `prepare_zero_rtt_fixture_dir`, потому key-file auth больше не является
  user-facing режимом.
- Добавлен `FPS_BUILD_FUZZERS=OFF` и пять clang/libFuzzer targets:
  TLS records, covert codec, FPS envelope, Zero-RTT candidates и TUN/control
  frame parsing. Seed corpora лежат в `tests/fuzz/corpus`, а quality script
  копирует их в build-dir перед запуском.
- `tools/run_quality_checks.sh --all` теперь включает короткий fuzz smoke;
  отдельный `--fuzz` оставлен для targeted reruns. Добавлены knobs
  `FPS_FUZZ_RUNS` и `FPS_FUZZ_SECONDS`.
- Добавлен `tools/docker_multi_client_sim.py`: один server, два UUID clients,
  distinct leases, UDP probes в обе стороны, spoofed source drop и post-spoof
  valid traffic check.
- `README.md`, `TESTING.md`, `docs/docker.md`, `ROADMAP.md` и `FPS_SPEC.md`
  обновлены под fuzzing/multi-client sanity workflow.

Решения:

- Основной multi-client product regression остается Docker opt-in, а не default
  local CTest, потому он проверяет реальные container namespaces, `/dev/net/tun`
  и lease routing.
- Fuzz smoke входит в `--all`, но остается bounded; long fuzz/minimization
  кампании вынесены в future CI/nightly.
- Низкие coverage зоны `tcp_relay_app.cpp`, `tcp_bridge_session.cpp` и Linux TUN
  runtime не признаны dead code: это operational/error/platform paths, которые
  покрываются интеграционными и opt-in TUN/Docker проверками хуже protocol core.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `tools/run_quality_checks.sh --all`
  - clang++-20 local suite: pass.
  - ASan+UBSan local suite: pass.
  - Valgrind unit pass: 0 errors, no leaks.
  - llvm-cov gate: line coverage 71.41% при minimum 70.00%, function coverage
    84.86% при minimum 80.00%.
  - libFuzzer smoke: 5 targets, 256 runs each, pass.
- `tools/run_quality_checks.sh --fuzz`
- `FPS_DOCKER_SUDO=1 tools/run_quality_checks.sh --docker`
- `FPS_DOCKER_SUDO=1 tools/docker_tun_iperf_sim.py --duration 10 --bandwidth 5M --length 1200`
  -> 0 lost / 5208 packets, 4.9994 Mbit/s, non-zero FPS TUN stats.
- `FPS_DOCKER_SUDO=1 tools/docker_socks_smoke.py --build`
  -> `socks_http_ok`.
- `FPS_DOCKER_SUDO=1 tools/docker_multi_client_sim.py --build`
  -> leases `10.89.0.2`/`10.89.0.3`, spoof drop event observed, post-spoof
  valid UDP pass, all services alive.
- `cmake -S . -B cmake-build-tun -DCMAKE_BUILD_TYPE=Debug -DFPS_ENABLE_TUN_TESTS=ON`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`

Заметки:

- Первый clean `ctest --test-dir build` один раз поймал transient
  `fps_https_zero_rtt_multi_session` TLS EOF; `ctest -R ... --repeat
  until-pass:3` и последующий полный rerun прошли. Это стоит помнить для
  будущего soak/retry hardening.
- Сеть не повреждалась; `NETWORK_RECOVERY.md` не обновлялся.

Commit:

- This logical commit: `Add fuzz smoke and multi-client sanity checks`

### Strict lease enforcement and Docker SOCKS sidecar

Цель:

- Сделать server-owned TUN leases настоящим contract: inbound source IP должен
  совпадать с lease, а server-to-client packets должны уходить только в carrier
  владельца destination lease.
- Добавить operator tooling для просмотра/revoke/prune lease file без вывода
  UUID и full key material.
- Добавить optional SOCKS5 profile в Docker image через Dante sidecar.

Сделано:

- `SessionManager` получил lease-aware carrier metadata: server-side outbound
  TUN packets routed by IPv4 destination lease, несколько carriers одного
  клиента используются round-robin, inbound client packets проходят strict
  source-IP enforcement после fragment reassembly.
- Server relay выделяет lease до carrier registration; при ошибке allocator-а
  session не становится TUN carrier.
- `TunLeaseAllocator` получил list/remove/prune APIs и atomic save через temp
  file + rename; lease file по-прежнему хранит только public-key base64 -> IP.
- `fps_server` получил команды `--lease-list`,
  `--lease-revoke-client-uuid UUID` и `--lease-prune`; stdout - JSON без UUID и
  full public/private keys.
- Docker runtime получил `dante-server` и `fps-socks-entrypoint.sh`; server
  compose добавил optional `socks` profile с sidecar
  `network_mode: "service:fps-server"`.
- Добавлен opt-in `tools/docker_socks_smoke.py`: FPS server/client/carrier,
  Dante sidecar и HTTP origin, затем SOCKS5 TCP connect через TUN.
- `FPS_SPEC.md`, `README.md`, `docs/docker.md`, `TESTING.md`, `ROADMAP.md`
  обновлены под strict leases, lease tooling и SOCKS5 sidecar.

Решения:

- SOCKS5 выбран через Dante, потому это прямой SOCKS daemon без дополнительной
  encrypted proxy модели; Shadowsocks оставлен вне scope.
- SOCKS запускается sidecar service из того же image, а не background process
  внутри FPS entrypoint.
- SOCKS first increment поддерживает TCP CONNECT без auth/ACL users/UDP
  ASSOCIATE; доступ ограничивается private TUN CIDR.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `docker compose -f examples/docker/server/compose.yml --profile socks config`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `FPS_DOCKER_SUDO=1 tools/run_quality_checks.sh --docker`
- `FPS_DOCKER_SUDO=1 tools/docker_tun_iperf_sim.py --duration 10 --bandwidth 5M --length 1200`
  -> 0 lost / 5208 packets, 4.9995 Mbit/s, all services running, non-zero FPS
  TUN stats.
- `FPS_DOCKER_SUDO=1 tools/docker_socks_smoke.py --build`
  -> `socks_http_ok`, all six services running after probe.
- `git diff --check`

Commit:

- This logical commit: `Enforce leased TUN addresses and add Docker SOCKS sidecar`

### Refactor config identity and Linux runtime boundary

Цель:

- Сузить production auth config до client UUID и server inline base64 X25519
  keypair + `allowed_client_uuids`.
- Перевести JSON parsing с Boost.PropertyTree на Boost.JSON и убрать stale
  key-file/client-key/public-key allowlist режимы без compatibility layer.
- Отделить platform-neutral core от Linux runtime, чтобы Android позже мог
  переиспользовать crypto/session/TUN framing без Linux `ip`/`/dev/net/tun`
  обвязки.

Сделано:

- `fps_core` больше не включает relay CLI app и Linux TUN open; добавлен
  отдельный `fps_linux_runtime` для `fps_client`/`fps_server`.
- Добавлен injectable `TunRuntime`: production Linux implementation открывает
  TUN и запускает `ip` через fork/exec без shell, unit tests используют fake
  runtime для проверки preconfigure/apply lease команд.
- Config loader и shaper/lease JSON parsing переведены на Boost.JSON с typed
  helper diagnostics; `tun_lease` load/save теперь тоже использует Boost.JSON.
- Zero-RTT config принимает только:
  - client: `client_uuid`, `server_public_key_base64`;
  - server: `server_private_key_base64`, `server_public_key_base64`,
    `allowed_client_uuids`.
- Removed fields (`*_key_file`, explicit client keys, allowed client public-key
  fields) дают clear config error даже если `security.zero_rtt.enabled` не
  задан.
- CLI оставляет `--generate-client-uuid` и заменяет raw key-file helpers на
  `--generate-server-keypair`; obsolete key-tool options возвращают parse error.
- Docker examples больше не монтируют `keys/`; configs используют inline
  placeholder base64 fields, а Dockerfile получил Boost.JSON build/runtime deps.

Решения:

- `client_public_key` остается внутренней runtime identity деталью Zero-RTT и
  lease file (`client_public_key` base64 -> IP), но не user-facing auth config.
- Server config JSON содержит private key material; Docker docs явно трактуют
  mounted config как secret.
- Android boundary сейчас является C++ seam, не Android implementation:
  `VpnService` fd и network configurator будут подставляться позже.
- Strict source-IP enforcement и lease-management tooling остаются следующим
  hardening инкрементом.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `FPS_DOCKER_SUDO=1 tools/run_quality_checks.sh --docker`
- `FPS_DOCKER_SUDO=1 tools/docker_tun_iperf_sim.py --duration 10 --bandwidth 5M --length 1200`
  -> 0 lost / 5208 packets, 4.9995 Mbit/s, all services running, non-zero FPS
  TUN stats.
- `git diff --check`

Commit:

- This logical commit: `Refactor config identity and Linux runtime boundary`

### UUID identity and server-assigned TUN leases

Цель:

- Упростить клиентский UX до одного secret UUID и убрать необходимость client key
  files в типовом сценарии.
- Добавить server-owned IPv4 lease allocator вместо DHCP-over-L3-TUN и дать
  клиенту opt-in `tun.auto_configure`.
- Сохранить advanced key-file/base64 режимы, но не логировать UUID, private keys,
  ClientID или raw control/TUN payload bytes.

Сделано:

- Добавлен `fps/core/identity`: canonical v4 UUID parser/generator,
  deterministic UUID -> X25519 keypair derivation, padded RFC4648 base64 helpers.
- Config/CLI поддерживают `security.zero_rtt.client_uuid`,
  `allowed_client_uuids`, inline `*_key_base64`, `--generate-client-uuid` и
  `--print-public-key --client-uuid`.
- Добавлен encrypted `FrameType::control` и `tun_lease` payload:
  client/server/network IPv4, prefix и MTU.
- Серверный `TunLeaseAllocator` persist-ит public-key/base64 -> IPv4 mappings,
  исключает network/broadcast/server address и сообщает pool exhaustion.
- После Zero-RTT auth сервер отправляет lease control frame; клиент при
  `tun.auto_configure=true` поднимает link/MTU и применяет server-assigned IPv4.
- `tun.client_isolation=true` по умолчанию дропает packets, направленные на
  другой leased client address; strict source-IP enforcement оставлен future work.
- Исправлен порядок server confirmation/control frames: сначала mandatory
  Zero-RTT confirmation, затем callbacks/control; control frames обходят
  shaper-delay, чтобы не ломать implicit AEAD sequence order.
- Docker examples и `tools/docker_tun_iperf_sim.py` переведены на UUID +
  server-assigned client lease; server compose persist-ит `/var/lib/fps`.
- `FPS_SPEC.md`, `README.md`, `docs/docker.md`, `TESTING.md`, `ROADMAP.md`
  обновлены под UUID/lease baseline.

Решения:

- UUID является bearer secret, но не новой wire/config identity сущностью:
  under the hood используется тот же client public key и existing indexed
  Zero-RTT ClientID.
- Lease file не содержит секретов: только public key base64 и IPv4.
- Client-to-client isolation реализована внутри FPS drop-filter; spoofed source
  IP пока считается undefined behavior с будущим hardening.
- DHCP не возвращаем: текущий дизайн остается L3 TUN/control-plane.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `bash -n tools/*.sh docker/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `FPS_DOCKER_SUDO=1 tools/run_quality_checks.sh --docker`
- `FPS_DOCKER_SUDO=1 tools/docker_tun_iperf_sim.py --duration 10 --bandwidth 5M --length 1200`
  -> 0 lost / 5208 packets, 4.9995 Mbit/s, all services running, non-zero FPS
  TUN stats.
- `git diff --check`

Commit:

- This logical commit: `Add UUID identity and server-assigned TUN leases`

## 2026-05-12

### Docker TUN iperf simulation

Цель:

- Проверить production-like Docker runtime: `fps_client` и `fps_server` в
  отдельных контейнерных network namespaces, reusable `fps_carrier` как cover
  generator, UDP `iperf3` между двумя FPS TUN адресами.

Сделано:

- Добавлен `tools/docker_tun_iperf_sim.py`: ephemeral compose project, временные
  Zero-RTT keys/configs, `fpss0=10.88.0.1/30`, `fpsc0=10.88.0.2/30`, carrier
  origin/client и UDP `iperf3` over TUN.
- Docker runtime получил `iperf3` как operator/debug smoke tool; Docker quality
  check теперь запускает `iperf3 --version`.
- `TcpBridgeSession` собирает per-session traffic counters и relay логирует
  `event=session_stats` при закрытии carrier-сессии без payload/key bytes.
- Docs/TESTING/README описывают Docker TUN UDP simulation workflow.

Результаты симуляции:

- `250K`, 10s, UDP payload 512: 0 lost / 611 packets, 0.250 Mbit/s,
  jitter 15.15 ms; все 4 services остались running.
- `1M`, 10s, UDP payload 1024: 0 lost / 1221 packets, 1.000 Mbit/s,
  jitter 6.89 ms; все 4 services остались running.
- `5M`, 10s, UDP payload 1200: 0 lost / 5208 packets, 5.000 Mbit/s,
  jitter 4.03 ms; все 4 services остались running.
- `20M`, 10s, UDP payload 1200: 0 lost / 20832 packets, 19.998 Mbit/s,
  jitter 0.16 ms; все 4 services остались running.
- После intentional carrier-client stop оба relay записали ненулевые
  `event=session_stats`; для 20M server-side directional TUN frame/byte
  counters были ненулевыми.
- Артефакты последних прогонов лежат в ignored `captures/fps-iperf-*`; private
  ephemeral `*.key` файлы удалены/не сохраняются.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `bash -n tools/*.sh docker/*.sh`
- `cmake --build build -j 2`
- `ctest --test-dir build -R fps_unit_tests --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `FPS_DOCKER_SUDO=1 tools/run_quality_checks.sh --docker`
- `FPS_DOCKER_SUDO=1 tools/docker_tun_iperf_sim.py --build --duration 10 --bandwidth 250K --length 512 --keep-artifacts`
- `FPS_DOCKER_SUDO=1 tools/docker_tun_iperf_sim.py --duration 10 --bandwidth 1M --length 1024 --carrier-bps 1048576 --keep-artifacts`
- `FPS_DOCKER_SUDO=1 tools/docker_tun_iperf_sim.py --duration 10 --bandwidth 5M --length 1200 --carrier-bps 2097152 --keep-artifacts`
- `FPS_DOCKER_SUDO=1 tools/docker_tun_iperf_sim.py --duration 10 --bandwidth 20M --length 1200 --carrier-bps 4194304 --keep-artifacts`
- `FPS_DOCKER_SUDO=1 tools/docker_tun_iperf_sim.py --duration 3 --bandwidth 500K --length 512`
- `git diff --check`

Commit:

- `8bbea57 Add Docker TUN iperf simulation`

### Use websockets for reusable WSS carrier

Цель:

- Упростить `fps_carrier`, убрав ручной WebSocket handshake/framing/masking в
  пользу проверенной Python dependency.

Сделано:

- Добавлен `requirements-runtime.txt` с pinned `websockets`.
- `tools/fps_carrier.py` переписан на async `websockets.serve/connect`; binary
  deterministic payload validation сохранен.
- Docker runtime больше не оптимизируется под `python3-minimal`: устанавливает
  `python3`, временно `python3-pip`, pinned dependency и удаляет pip после
  установки.
- `tools/run_quality_checks.sh --python` проверяет import `websockets`.
- README, TESTING и Docker docs обновлены под pip dependency.

Проверка:

- `sudo -n python3 -m pip install --break-system-packages -r requirements-runtime.txt`
- `python3 -m py_compile tests/integration/*.py tools/*.py`
- `python3 -c "import websockets; print(websockets.__version__)"`
- `python3 tools/fps_carrier.py --help`
- `python3 tests/integration/carrier_tool.py --carrier /workspaces/tools/fps_carrier.py`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `python3 tests/integration/wss_passthrough.py --fps-server ./build/fps_server --carrier /workspaces/tools/fps_carrier.py`
- `python3 tests/integration/wss_zero_rtt_chain.py --fps-client ./build/fps_client --fps-server ./build/fps_server --carrier /workspaces/tools/fps_carrier.py`
- `cmake --build build -j 2`
- `ctest --test-dir build -L local --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `tools/run_quality_checks.sh --python`
- `FPS_DOCKER_SUDO=1 tools/run_quality_checks.sh --docker`
- `git diff --check`

Commit:

- `b245584 Use websockets for WSS carrier`

### Add reusable WSS carrier generator

Цель:

- Добавить общий `fps_carrier` как debug carrier generator для Docker runtime и
  интеграционных тестов, чтобы не держать несколько одноразовых WSS/cover
  реализаций.

Сделано:

- Добавлен `tools/fps_carrier.py` с ролями `origin` и `client`: TLS WSS,
  self-signed cert generation через `openssl`, deterministic binary frames,
  reconnect loop и test controls.
- CMake install кладет tool как `fps_carrier`; Docker runtime включает
  `python3-minimal`/`openssl`, а Docker smoke запускает `fps_carrier --help`.
- Добавлен local regression `fps_carrier_tool`.
- WSS passthrough/Zero-RTT tests и TUN loopback family переведены на reusable
  carrier tool; HTTPS GET tests оставлены отдельным request/response сигналом.
- Добавлен `examples/docker/debug-carrier` с отдельными services для carrier
  origin/client и FPS relay pair.
- README, TESTING, Docker docs, ROADMAP и FPS_SPEC обновлены.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/*.py && bash -n tools/*.sh docker/*.sh`
- `python3 tools/fps_carrier.py --help`
- `python3 tests/integration/carrier_tool.py --carrier /workspaces/tools/fps_carrier.py`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `docker compose -f examples/docker/server/compose.yml config`
- `docker compose -f examples/docker/client-host/compose.yml config`
- `docker compose -f examples/docker/debug-carrier/compose.yml config`
- `cmake --build build -j 2`
- `ctest --test-dir build -R 'fps_carrier_tool|fps_wss_passthrough|fps_wss_zero_rtt_chain|fps_docker_artifacts' --output-on-failure`
- `cmake --install build --prefix /tmp/fps-install-carrier-check`
- `ctest --test-dir build -L local --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -R 'fps_tun_zero_rtt_loopback' --output-on-failure`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `FPS_DOCKER_SUDO=1 tools/run_quality_checks.sh --docker`
- `git diff --check`

Commit:

- `162b4cd Add reusable WSS carrier generator`

### Remove native packaging deployment surface

Цель:

- Упростить product/deployment story: Docker Runtime остается единственным
  поддерживаемым Linux deployment path, а native packaging/systemd examples
  удаляются как не дающие killer-feature относительно Docker.

Сделано:

- Удалены `packaging/systemd`, `packaging/sysusers.d`, `packaging/tmpfiles.d` и
  `docs/systemd.md`.
- Удален regression `fps_systemd_examples` и его регистрация в CMake.
- README, ROADMAP, TESTING, REVIEW и FPS_SPEC обновлены: текущие docs больше не
  рекламируют native packaging/systemd как вариант развертывания.
- CMake `install()` rules сохранены, потому Docker build использует install
  layout как staging-механизм для runtime image.

Проверка:

- `python3 -m py_compile tests/integration/*.py && bash -n tools/*.sh docker/*.sh`
- `rg -n "packag|systemd|sysusers|tmpfiles|native" README.md ROADMAP.md TESTING.md REVIEW.md FPS_SPEC.md docs CMakeLists.txt tests/integration tools examples -S`
- `cmake --build build -j 2`
- `ctest --test-dir build -L local --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`

Commit:

- `1109db4 Remove native packaging deployment surface`

## 2026-05-11

### Add Docker runtime packaging baseline

Цель:

- Перенести основной Linux deployment path с native packaging на Docker runtime
  baseline: один общий image, compose examples и opt-in Docker quality check.

Сделано:

- Добавлен multi-stage `Dockerfile` на `ubuntu:24.04`: builder собирает Release
  binaries через CMake install target, runtime содержит FPS binaries,
  `fps_linux_route.sh`, `iproute2`, `ca-certificates`, `tini` и runtime libs.
- Добавлен `docker/fps-entrypoint.sh` с `FPS_ROLE`, `FPS_CONFIG`,
  `FPS_LOG_LEVEL`, `FPS_CONFIGURE_TUN`, `FPS_TUN_NAME`, `FPS_TUN_ADDRESS`,
  `FPS_TUN_MTU`, `FPS_TUN_ROUTES`.
- Добавлены compose examples для server bridge deployment и host-network client
  deployment с `/dev/net/tun`, `NET_ADMIN`, `NET_BIND_SERVICE`, read-only
  config/key mounts и без `privileged: true`.
- Добавлен `docs/docker.md`; README/TESTING/ROADMAP обновлены под Docker-first
  deployment.
- Добавлен static regression `fps_docker_artifacts`; `tools/run_quality_checks.sh`
  получил opt-in `--docker`.

Решения:

- SOCKS5 не включен в первый Docker image; следующий Docker subtask - optional
  SOCKS5 server profile, вероятно через Dante package.
- DHCP отложен: текущий FPS - L3 TUN, обычный DHCP ожидает L2/broadcast; для
  адресации нужен отдельный L3 allocator/control-plane или TAP research.
- Client compose использует `network_mode: host`, потому host-side TUN routes
  иначе не появятся в host namespace.

Проверка:

- `python3 -m py_compile tests/integration/*.py && bash -n tools/*.sh docker/*.sh`
- `python3 tests/integration/docker_artifacts.py --repo /workspaces`
- `docker compose -f examples/docker/server/compose.yml config`
- `docker compose -f examples/docker/client-host/compose.yml config`
- `cmake --build build -j 2`
- `ctest --test-dir build -R 'fps_docker_artifacts|fps_systemd_examples|fps_linux_route_helper|fps_unit_tests' --output-on-failure`
- `cmake --install build --prefix /tmp/fps-install-check`
- `tools/run_quality_checks.sh --python`
- `ctest --test-dir build -L local --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `tools/run_quality_checks.sh --python --clang`
- `FPS_DOCKER_SUDO=1 tools/run_quality_checks.sh --docker`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`

Наблюдения:

- Непривилегированный `docker` в текущей VM не имеет доступа к
  `/var/run/docker.sock`; opt-in Docker check успешно проходит через
  `FPS_DOCKER_SUDO=1`.

Commit:

- `490e9fd Add Docker runtime packaging baseline`

### Phase 1 quality sweep

Цель:

- После закрытия Linux Runtime Basics прогнать полный non-sudo quality set,
  включая sanitizers, Valgrind и coverage gate.

Проверка:

- `tools/run_quality_checks.sh --all`

Результат:

- clang++-20 local suite: pass.
- ASan+UBSan local suite: pass.
- Valgrind unit pass: 0 errors, no leaks.
- llvm-cov gate: line coverage 71.80% при minimum 70.00%, function coverage
  84.45% при minimum 80.00%.

### Add systemd deployment examples

Цель:

- Завершить Phase 1 Linux Runtime Basics упаковочными примерами для запуска
  `fps_client`/`fps_server` как Linux services.

Сделано:

- Добавлены templated units `packaging/systemd/fps-client@.service` и
  `packaging/systemd/fps-server@.service`.
- Добавлены `packaging/sysusers.d/fps.conf` и `packaging/tmpfiles.d/fps.conf`
  для dedicated `fps` user и базовых `/etc/fps`, `/var/lib/fps`, `/var/log/fps`
  directories.
- Добавлен `docs/systemd.md` с install layout, key/config preparation,
  `systemctl` commands и operational notes.
- Добавлен local regression `fps_systemd_examples`, который статически проверяет
  units/sysusers/tmpfiles без установки в систему и без запуска services.

Решения:

- Units не объявляют reload, потому runtime reload еще не реализован; после
  config/allowlist изменений нужен restart.
- Units дают `CAP_NET_ADMIN` для TUN и `CAP_NET_BIND_SERVICE` для низких портов,
  но не настраивают routes/DNS/NAT/firewall.
- Config/key ownership в документации оставлен root:fps; private keys читаются
  group `fps`, но не должны быть writable service user-ом.

Проверка:

- `python3 -m py_compile tests/integration/*.py && bash -n tools/*.sh`
- `python3 tests/integration/systemd_examples.py --repo /workspaces`
- `cmake --build build -j 2`
- `ctest --test-dir build -R 'fps_systemd_examples|fps_linux_route_helper|fps_cli_streams|fps_unit_tests' --output-on-failure`
- `tools/run_quality_checks.sh --python`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `tools/run_quality_checks.sh --python --clang`
- `cmake --build cmake-build-tun -j 2`
- `ctest --test-dir cmake-build-tun -R 'fps_systemd_examples|fps_linux_route_helper' --output-on-failure`
- `git diff --check`

Commit:

- `4b92497 Add systemd deployment examples`

### Add Linux route/DNS workflow helper

Цель:

- Продолжить Phase 1 productization: дать безопасный repeatable workflow для
  адресов TUN, split/full tunnel routing, DNS и cleanup без скрытых изменений
  host networking.

Сделано:

- Добавлен `tools/fps_linux_route.sh` с actions `plan|apply|cleanup`.
- Helper умеет настраивать TUN address/MTU/link-up, split routes, resolvectl
  DNS/domains, full-tunnel policy table через explicit `--fwmark` или `--from`,
  carrier bypass routes и cleanup/dry-run.
- Добавлен `docs/linux-routing.md` с split tunnel, full tunnel fwmark/nftables,
  bypass и cleanup examples.
- Добавлен local regression `fps_linux_route_helper`, который проверяет dry-run
  output для split/full/cleanup и не меняет host routes/DNS.
- `tools/run_quality_checks.sh --python` теперь также запускает `bash -n` для
  shell tools.

Решения:

- Helper не стартует FPS daemons, не включает forwarding, не создает NAT и не
  меняет firewall/nftables; эти операции зависят от deployment и должны идти
  отдельным ревьюируемым плейбуком.
- Full tunnel требует явный policy selector, чтобы случайно не завернуть весь
  host и carrier TCP-сессию внутрь самого FPS TUN.

Проверка:

- `python3 -m py_compile tests/integration/*.py && bash -n tools/*.sh`
- `tools/fps_linux_route.sh plan --tun fpsc0 --tun-address 10.66.0.2/30 --mtu 1280 --route 10.66.1.0/24 --dns 10.66.0.1 --dns-domain '~fps.test'`
- `tools/fps_linux_route.sh plan --tun fpsc0 --full-tunnel --table 100 --priority 10000 --fwmark 0x465053 --bypass 203.0.113.10/32,192.0.2.1,eth0`
- `cmake --build build -j 2`
- `ctest --test-dir build -R 'fps_linux_route_helper|fps_cli_streams|fps_unit_tests' --output-on-failure`
- `tools/run_quality_checks.sh --python`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `tools/run_quality_checks.sh --python --clang`
- `cmake --build cmake-build-tun -j 2`
- `ctest --test-dir cmake-build-tun -R fps_linux_route_helper --output-on-failure`
- `git diff --check`

Commit:

- `6517cf3 Add Linux route and DNS workflow helper`

### Add Linux key/config CLI tooling

Цель:

- Начать Phase 1 productization с operator-friendly key/config workflow для
  Linux client/server.

Сделано:

- `fps_client` и `fps_server` получили общие CLI-команды:
  `--generate-keypair`, `--print-public-key`, `--check-keypair` и
  `--check-config`.
- Key generation создает raw 32-byte X25519 private/public key files без
  перезаписи существующих файлов; private key получает mode `0600`, public key -
  `0644`.
- `--print-public-key` выводит hex public key, пригодный для inline server
  allowlist config; `--check-keypair` проверяет соответствие private/public key.
- `--check-config` валидирует JSON и печатает однострочный non-secret summary с
  role/listen/target/log/tun/zero-rtt/shaper metadata.
- Regression suite `fps_cli_streams` расширен проверками stdout/stderr,
  permissions, key mismatch diagnostics и `--log-level` override для
  `--check-config`.

Решения:

- Key/config helper commands завершаются до инициализации runtime logger, чтобы
  stdout/stderr оставались предсказуемыми для shell scripts.
- Существующие key files не перезаписываются; если запись public key падает
  после private key, private key удаляется best-effort.
- Public key не считается секретом и может печататься в stdout; private key,
  raw secret material и config secret bytes не логируются и не выводятся.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `ctest --test-dir build -R 'fps_unit_tests|fps_cli_streams' --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `tools/run_quality_checks.sh --python --clang`
- `git diff --check`

Commit:

- `a90049e Add Linux key and config CLI tooling`

### Clean rebuild and product roadmap

Цель:

- Полностью удалить build directories, заново собрать все рабочие варианты и
  проверить, что тесты не зависят от старых бинарников или CMake cache.
- Сформулировать следующий productization plan с shaper как deferred feature.

Сделано:

- Удалены `build` и все `cmake-build-*`.
- С нуля собраны и проверены обычный Debug build с pcap tests, TUN build,
  clang-20 build, ASan/UBSan build, Valgrind build и coverage build.
- Добавлен `ROADMAP.md` с Linux-first планом: runtime basics, operations,
  security hardening, packaging/compatibility и deferred advanced shaping.

Решения:

- Shaper mechanics не является ближайшим blocker для рабочего продукта; текущий
  приоритет - надежный Linux TUN tunnel, key/config tooling, route/service
  workflow, observability и deployment.
- Pcap/wire-shape и Zero-RTT/envelope tests остаются обязательной защитой от
  грубых wire-regressions до возвращения к advanced shaping.

Проверка:

- `rm -rf build cmake-build-*`
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DFPS_BUILD_TESTS=ON -DFPS_ENABLE_PCAP_TESTS=ON`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake -S . -B cmake-build-tun -DCMAKE_BUILD_TYPE=Debug -DFPS_BUILD_TESTS=ON -DFPS_ENABLE_TUN_TESTS=ON`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `tools/run_quality_checks.sh --all`
- `sudo -n ctest --test-dir cmake-build-tun --output-on-failure`
- `git diff --check`
- `ip netns list`
- `pgrep -af 'fps_client|fps_server|tcpdump|valgrind|llvm-cov|llvm-profdata'`

Commit:

- `bbb3b38 Record clean rebuild and product roadmap`

### Add llvm-cov coverage gate

Цель:

- Добавить coverage в постоянный quality set, чтобы исчезновение тестов или
  выпадение сценариев из local regression suite было видно раньше.

Сделано:

- `tools/run_quality_checks.sh` получил режим `--coverage` и включает его в
  `--all`.
- Coverage build использует `clang++-20`, `-fprofile-instr-generate`,
  `-fcoverage-mapping`, `llvm-profdata-20` и `llvm-cov-20`.
- Скрипт генерирует `coverage-summary.txt`, `coverage-summary.json` и HTML report
  в `cmake-build-coverage/coverage-html/index.html`.
- Добавлен coverage gate: total lines >= 70%, total functions >= 80%;
  переопределяется через `FPS_COVERAGE_MIN_LINES` и
  `FPS_COVERAGE_MIN_FUNCTIONS`.
- По результатам первого отчета удален устаревший неиспользуемый
  `hmac_sha256` helper и добавлены unit-тесты для SHA-256/HKDF/AEAD error path,
  `constant_time_equal`, logging severity strings, sequence/pending accessors и
  parser reset.
- `README.md` и `TESTING.md` обновлены coverage workflow и текущим baseline.

Решения:

- Coverage остается non-sudo local check; TUN/netns coverage не входит в
  `--coverage`, чтобы quality suite можно было запускать без root.
- Текущий baseline: 72.69% line coverage и 84.59% function coverage.
- Ожидаемые gaps: `src/platform/linux/tun_device.cpp` 0% в non-sudo режиме;
  основные оставшиеся непокрытые строки в `tcp_relay_app.cpp`/`tcp_bridge_session.cpp`
  относятся к operational/error/shaper/TUN callback веткам.

Проверка:

- `bash -n tools/run_quality_checks.sh && tools/run_quality_checks.sh --coverage`
- `tools/run_quality_checks.sh --all`
- `cmake --build build -j 2`
- `ctest --test-dir build -R fps_unit_tests --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`

Commit:

- `18c5a66 Add llvm-cov coverage gate`

### Add repeatable sanitizer quality checks

Цель:

- Перепроверить текущий baseline через clang++-20, ASan/UBSan и Valgrind, а
  также оставить воспроизводимый non-sudo workflow для периодических прогонов.

Сделано:

- Добавлен `tools/run_quality_checks.sh` с режимами `--python`, `--clang`,
  `--sanitizers`, `--valgrind` и `--all`.
- Проверки запускаются в отдельных build directories:
  `cmake-build-clang20`, `cmake-build-asan-ubsan`, `cmake-build-valgrind`.
- `README.md` и `TESTING.md` обновлены командами запуска и scope каждого режима.

Решения:

- clang path использует `clang++-20`, но допускает override через `CLANG_CXX`.
- ASan+UBSan покрывает local CTest, включая Python integration tests.
- Valgrind оставлен на `fps_unit_tests`, чтобы сигнал был быстрым и не зависел от
  сетевого timing в integration tests.
- Static analyzer pass пока не добавлялся: для текущего инкремента baseline
  ограничен предупреждениями GCC/clang и runtime sanitizers.

Проверка:

- `bash -n tools/run_quality_checks.sh && tools/run_quality_checks.sh --help`
- `tools/run_quality_checks.sh --python --clang`
- `tools/run_quality_checks.sh --sanitizers`
- `tools/run_quality_checks.sh --valgrind`

Commit:

- `58dd35b Add repeatable sanitizer quality checks`

### Add pcap TLS wire-shape regression

Цель:

- Снять pcap dumps для ручной проверки FPS link в Wireshark и добавить
  автоматическую проверку, что FPS traffic остается parseable TLS records.

Сделано:

- Добавлен `tools/is_pcap_looks_like_tls.py`: libpcap-backed offline checker,
  который реконструирует TCP payload streams и проверяет TLS record boundaries,
  content types и наличие Application Data без анализа таймингов/размеров.
- `tools/capture_tls_wire.sh` усилен для коротких тестов: `--immediate-mode`,
  `-U`, optional `aa-exec -p unconfined`, cleanup child tcpdump process.
- Добавлен opt-in CTest `fps_pcap_tls_shape` за флагом
  `-DFPS_ENABLE_PCAP_TESTS=ON`; тест снимает HTTPS Zero-RTT FPS link и прогоняет
  libpcap TLS-shape checker.
- `tun_zero_rtt_loopback.py` получил `--capture-pcap`, чтобы снимать underlay
  fps_client<>fps_server pcap внутри isolated netns после readiness probes.
- Сняты pcap artifacts для ручного открытия в Wireshark:
  `captures/fps_https_zero_rtt_chain.pcap` и
  `captures/fps_tun_zero_rtt_loopback.pcap`, рядом JSON summaries `*.shape.json`.
- Добавлен `NETWORK_RECOVERY.md` с фактическими recovery steps после зависшего
  AppArmor/tcpdump capture experiment.

Решения:

- `tshark` остается optional convenience, не baseline dependency.
- Regression check использует libpcap для чтения pcap; timing/size distribution
  analysis остается future shaper lab.
- `captures/` добавлен в `.gitignore`, pcap artifacts не коммитятся.

Проверка:

- `python3 -m py_compile tests/integration/*.py tools/is_pcap_looks_like_tls.py`
- `ctest --test-dir build -R fps_pcap_tls_shape --output-on-failure`
- `sudo -n python3 tests/integration/tun_zero_rtt_loopback.py --fps-client ./cmake-build-tun/fps_client --fps-server ./cmake-build-tun/fps_server --expect-carrier-count 1 --udp-count 8 --udp-payload-size 512 --capture-pcap /workspaces/captures/fps_tun_zero_rtt_loopback.pcap`
- `python3 tools/is_pcap_looks_like_tls.py captures/fps_tun_zero_rtt_loopback.pcap --require-bidirectional --require-application-data --min-records 4 --summary captures/fps_tun_zero_rtt_loopback.pcap.shape.json`

Commit:

- `87bd9d1 Add pcap TLS wire-shape checks`

### Add Zero-RTT indexed precheck

Цель:

- Убрать server-side `O(n)` allowlist trial decrypt из Zero-RTT upgrade без
  добавления ClientID в пользовательский config/API.

Сделано:

- `ZeroRttUpgradeEngine` теперь строит auth wire как
  `ephemeral_public[32] | precheck_box[32] | upgrade_ciphertext | upgrade_tag[16]`.
- `precheck_box` шифрует внутренний 16-byte ClientID, вычисленный из server public
  key, profile id и client public key; на проводе нет plaintext magic/tag.
- Server-side allowlist индексируется по derived ClientID; full upgrade decrypt
  выполняется только для найденного client public key.
- Добавлены typed errors `precheck_failed` и `unknown_client_id`; логи продолжают
  выводить только metadata-level error names.
- Config/CLI не получили новых ClientID-полей; `trial_decrypt_limit` оставлен
  совместимым config knob, но normal path больше не сканирует allowlist.
- Добавлен local integration `fps_https_zero_rtt_indexed_precheck` с decoy
  allowlist entries перед реальным ключом и `trial_decrypt_limit=1`.
- `FPS_SPEC.md`, `CRYPTO_REVIEW.md`, `README.md`, `TESTING.md` и `REVIEW.md`
  обновлены под implemented indexed precheck и остаточный CPU DoS риск.

Решения:

- ClientID остается implementation detail, а идентификация пользователя остается
  через обычные X25519 private/public keys и server allowlist.
- Precheck оптимизирует allowlist lookup до `O(1)`, но не вводит более дешевый
  pre-X25519 marker, чтобы не создавать стабильный wire-признак.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=zero_rtt_upgrade,fps_upgrade_controller,tcp_relay_app --catch_system_errors=no`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`
- `ip netns list | rg 'fps[cs]|fpsprobe' || true`

Commit:

- `09dd51e Add Zero-RTT indexed precheck`

### Review and clean v2-only protocol baseline

Цель:

- Перед дальнейшей production-разработкой синхронизировать код, спецификацию,
  regression docs и self-review с v2 Zero-RTT/envelope/carrier-pool моделью.

Сделано:

- Удален legacy IFF/shared-secret protocol из активной сборки: `IffEngine`,
  `IffSessionController`, IFF unit tests и IFF integration scripts больше не
  входят в проект.
- `TcpRelayConfig`/CLI/config теперь используют нейтральный `RelayRole`; TUN
  config требует `security.zero_rtt.enabled=true`, а `iff.enabled=true`
  возвращает parse error.
- Удалены public primary-session helpers из `SessionManager`; carrier pool API
  остается основным контрактом.
- CTest matrix переведена на passthrough + Zero-RTT: HTTPS/WSS local tests и
  Zero-RTT TUN loopback/burst/fragmentation/shaper/multi-carrier tests.
- `tun_iff_loopback.py` переименован и упрощен до `tun_zero_rtt_loopback.py`;
  `wss_iff_chain.py` заменен на `wss_zero_rtt_chain.py`; `tun_open_smoke.py`
  использует v2 config.
- `FPS_SPEC.md`, `README.md`, `TESTING.md`, `CRYPTO_REVIEW.md`, `REVIEW.md` и
  `research/README.md` актуализированы под v2-only baseline; stale
  `research/SPECS.md` удален.
- Добавлена `tools/capture_tls_wire.sh`: tcpdump capture с optional tshark TLS
  record summary для ручной проверки wire-shape.

Решения:

- Downgrade authenticated session не реализуем: carrier остается в envelope mode
  до close/drain.
- CPU-friendly Zero-RTT precheck/ClientID lookup зафиксирован как следующий
  crypto design item, но не реализован в cleanup-инкременте.
- `CovertCodec` остается как low-level/internal unit-test seam; relay
  authentication path теперь только Zero-RTT.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `tools/capture_tls_wire.sh --help`
- `git diff --check`
- `ip netns list | rg 'fps[cs]|fpsprobe' || true`

Commit:

- `aef35f6 Clean up v2-only protocol baseline`

### Add Zero-RTT TUN/netns integration coverage

Цель:

- Доказать v2 `security.zero_rtt` path не только на HTTPS passthrough, но и на
  real Linux TUN traffic в изолированных network namespaces.

Сделано:

- `tun_iff_loopback.py` расширен параметром `--protocol iff|zero_rtt`; default
  остался IFF, поэтому прежние TUN tests не меняют invocation.
- Для `zero_rtt` сценарий пишет test-only raw X25519 key files, генерирует
  `security.zero_rtt` configs и запускает те же client/server netns, veth
  underlay, real TUN devices и UDP echo.
- Добавлен `--carrier-count` и log assertion `--expect-carrier-count`, чтобы
  проверять регистрацию нескольких authenticated carriers в relay pool.
- CTest получил `fps_tun_zero_rtt_loopback` и
  `fps_tun_zero_rtt_multi_carrier`; второй держит два cover TLS carriers и
  гонит burst UDP echo через TUN.
- README/TESTING/CRYPTO_REVIEW обновлены: v2 TUN/netns coverage теперь есть,
  shaper+Zero-RTT+multi-carrier остается отдельным gap.

Решения:

- Переиспользуем существующий isolated netns harness без global routes/NAT/firewall.
- Тест проверяет carrier registration по operational logs, но не логирует raw
  payload, keys или packet bytes.
- Один ручной параллельный запуск single и multi сценариев дал transient early
  cover-client exit в single run; последовательный manual run и CTest run
  прошли. CTest запускает эти сценарии последовательно.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n python3 tests/integration/tun_iff_loopback.py --fps-client ./cmake-build-tun/fps_client --fps-server ./cmake-build-tun/fps_server --protocol zero_rtt --expect-carrier-count 1`
- `sudo -n python3 tests/integration/tun_iff_loopback.py --fps-client ./cmake-build-tun/fps_client --fps-server ./cmake-build-tun/fps_server --protocol zero_rtt --carrier-count 2 --expect-carrier-count 2 --udp-count 16 --udp-payload-size 512`
- `sudo -n ctest --test-dir cmake-build-tun -R 'fps_tun_zero_rtt_(loopback|multi_carrier)' --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`
- `ip netns list | rg 'fps[cs]|fpsprobe' || true`

Commit:

- `2b429e8 Add Zero-RTT TUN integration coverage`

### Add Zero-RTT HTTPS integration coverage

Цель:

- Вывести v2 Zero-RTT/envelope path из unit-only зоны в local end-to-end
  regression suite без root/TUN.

Сделано:

- `fps_https_harness.py` получил helper-ы для test-only raw X25519 key files и
  JSON config с `security.zero_rtt`.
- Добавлен `fps_https_zero_rtt_chain`: реальный HTTPS keep-alive через
  `fps_client -> fps_server -> origin` с late Zero-RTT upgrade, server
  first-response confirmation и encrypted envelope mode.
- Добавлен `fps_https_zero_rtt_multi_session`: две одновременные browser/origin
  TLS sessions через v2 path без смешивания HTTP responses.
- CTest получил label `zero_rtt`; ordinary local `ctest` включает новые v2
  scenarios.
- README/TESTING/CRYPTO_REVIEW обновлены под новый integration baseline.

Решения:

- Local no-root v2 tests не включают `tun.enabled`, поэтому relay не создает
  `SessionManager` и не логирует `carrier_registered`; это останется для
  отдельного v2 TUN/netns harness.
- Тестовые X25519 ключи raw 32-byte и детерминированы только для integration
  fixtures; логи проверяются на отсутствие raw payload/session key/private-key
  markers.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `ctest --test-dir build -L zero_rtt --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`
- `ip netns list | rg 'fps[cs]|fpsprobe' || true`

Commit:

- `1ff04ca Add Zero-RTT HTTPS integration coverage`

## 2026-05-10

### Add CarrierPool scheduling for TUN packets

Цель:

- Убрать single-primary bottleneck из TUN injection path и подготовить relay к
  v2 multi-carrier модели, где любая authenticated TLS session может переносить
  encrypted FPS frames.

Сделано:

- `SessionManager` получил carrier-pool API:
  `add_carrier_session`, `is_carrier_session`, `remove_carrier_session_if`,
  `clear_carrier_sessions`, `carrier_count`.
- `handle_tun_packet(...)` теперь выбирает live carrier round-robin, пропускает
  expired/closed sessions и пробует следующий carrier при `write_queue_full`.
- Fragmented TUN packets по-прежнему enqueue'ятся batch-ом в один выбранный
  carrier, поэтому partial fragment write не появляется.
- Старые primary helper-ы оставлены как transitional aliases, чтобы существующий
  TUN pump/test harness не ломался во время поэтапной миграции.
- Relay регистрирует IFF и Zero-RTT authenticated sessions как carriers, принимает
  inbound TUN frames от любого registered carrier и снимает carrier on close.
- `tun.enabled` теперь разрешен с `iff.enabled` или с `security.zero_rtt.enabled`;
  `codec.max_frame_payload`/`max_frame_padding` стали общими relay-level limits
  для IFF и v2 envelope frames.
- README/TESTING/CRYPTO_REVIEW обновлены под no-primary carrier pool.

Решения:

- При saturated carrier `SessionManager` пробует следующий session; если все
  carriers saturated, возвращается bounded `write_queue_full`.
- При отсутствии carrier возвращается новый `no_carrier_session`; старый
  `no_primary_session` оставлен enum alias для переходной совместимости.
- Unit coverage пока доказывает round-robin/fallback socket-free; real netns TUN
  интеграция остается IFF-based до отдельного v2 harness.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=session_manager,tun_packet_pump,tcp_relay_app --catch_system_errors=no`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`
- `ip netns list | rg 'fps[cs]|fpsprobe' || true`

Commit:

- `e160579 Add carrier pool TUN scheduling`

### Add Zero-RTT first-response key confirmation

Цель:

- Убрать локальную client-side "auth сразу после build upgrade" семантику и
  требовать подтверждение, что peer действительно смог вывести session keys.

Сделано:

- Server-side `TcpBridgeSession` после successful v2 upgrade отправляет первый
  server-to-client encrypted envelope без inner TLS/covert payload как key
  confirmation.
- Client-side `TcpBridgeSession` после отправки late upgrade record держит
  client-to-server чтение на паузе и помечает v2 auth только после успешного
  decrypt полного server-to-client envelope record.
- Неполное TLS record confirmation буферизуется без auth; decrypt/parse/record
  errors закрывают session без выдачи raw bytes.
- Socket-level tests покрывают server confirmation, client wait-for-confirmation,
  fragmented confirmation и post-confirmation envelope wrapping.

Решения:

- Confirmation на этом шаге - пустой encrypted envelope. Позже его можно
  совместить с первым server inner TLS/control frame, не меняя implicit-sequence
  envelope модель.
- Если client получает local plaintext в upgrade direction до confirmation,
  bridge закрывается вместо молчаливой потери байтов.
- Секреты, ключи, raw TLS payload и envelope plaintext не логируются.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=tcp_bridge_session --catch_system_errors=no`
- `./build/fps_unit_tests --run_test=tcp_bridge_session,fps_upgrade_controller --catch_system_errors=no`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`
- `ip netns list | rg 'fps[cs]|fpsprobe' || true`

Commit:

- `9ca737e Add Zero-RTT key confirmation`

### Wire Zero-RTT v2 into TCP bridge

Цель:

- Подключить уже готовые `FpsUpgradeController` и `FpsEnvelopePipeline` к
  `TcpBridgeSession`, чтобы live bridge мог переходить из passthrough в v2
  envelope mode без MVP IFF.

Сделано:

- `TcpBridgeSessionConfig` получил optional `TcpBridgeZeroRttOptions`.
- Client role наблюдает TLS record boundaries, после первого полного record
  вставляет late zero-RTT upgrade record и переключает дальнейшие local TLS bytes
  в encrypted envelope records.
- Server role trial-decrypt'ит plausible application-data records, strip'ит
  успешный upgrade record и после auth unwrap'ит incoming envelope records в inner
  TLS bytes + covert frames.
- После v2 auth external `enqueue_covert_frame(s)` кодирует frames как encrypted
  envelope records без plaintext frame metadata.
- Relay теперь передает распарсенный `security.zero_rtt` в `TcpBridgeSession` и
  логирует v2 auth/build/upgrade/envelope events без raw payload/key material.
- `FpsUpgradeController` после successful upgrade больше не forward'ит
  subsequent records из того же parse batch как plaintext, чтобы не протекали
  post-upgrade envelope bytes в origin.
- Добавлены socket-level тесты на server-side strip+unwrap и client-side late
  upgrade+wrap.
- README/TESTING обновлены: v2 bridge path уже подключен, TUN carrier-pool still
  pending.

Решения:

- В этом инкременте client считает себя authenticated сразу после успешного
  local build upgrade record; явная server key-confirmation остается следующим
  crypto/protocol шагом.
- Envelope mode считает plaintext/non-application TLS records после upgrade
  protocol error и закрывает bridge.
- CarrierPool/no-primary пока не затронут: v2 auth callback временно использует
  существующий primary assignment hook для будущего TUN перехода.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=tcp_bridge_session --catch_system_errors=no`
- `./build/fps_unit_tests --run_test=tcp_bridge_session,fps_upgrade_controller --catch_system_errors=no`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`
- `ip netns list | rg 'fps[cs]|fpsprobe' || true`

Commit:

- `1af33ae Wire Zero-RTT v2 into TCP bridge`

### Add FPS v2 envelope TLS pipeline

Цель:

- Добавить socket-free слой после upgrade, который оборачивает encrypted FPS
  envelopes в TLS application-data records и распаковывает их обратно перед
  интеграцией в `TcpBridgeSession`.

Сделано:

- Добавлен `FpsEnvelopePipeline`: `encode_tls_record(...)` строит
  `FpsEnvelopeCodec` ciphertext и заворачивает его в TLS application-data record.
- `process_inbound_tls(...)` буферизует fragmented TLS records, декодирует
  envelopes, возвращает inner real TLS bytes и covert frames.
- Tamper/decrypt failure и non-application records после upgrade помечаются
  `close_required=true` без выдачи inner bytes.
- Добавлены unit-тесты на roundtrip, fragmented boundary buffering, tamper close,
  non-application close и surfacing encode errors.
- README/TESTING обновлены для нового v2 pipeline слоя.

Решения:

- После v2 upgrade pipeline ожидает только encrypted envelope records; plaintext
  TLS records после upgrade считаются protocol error/close signal.
- Sequence остается implicit внутри `FpsEnvelopeCodec`; pipeline не добавляет
  plaintext metadata вокруг envelope payload.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=fps_envelope_pipeline --catch_system_errors=no`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`
- `ip netns list | rg 'fps[cs]|fpsprobe' || true`

Commit:

- `eb3bbd4 Add FPS v2 envelope pipeline`

### Add relay config support for Zero-RTT v2

Цель:

- Зафиксировать валидируемый JSON-контракт `security.zero_rtt.*` перед
  подключением v2 upgrade/envelope path к живому `TcpBridgeSession`.

Сделано:

- `TcpRelayConfig` получил optional `ZeroRttRelayConfig` с
  `FpsUpgradeControllerConfig`.
- Config loader читает role-specific raw 32-byte X25519 key files:
  client private/public + server public для client role, server private/public +
  allowlist client public keys для server role.
- Loader проверяет размер key files, соответствие public/private keypair,
  обязательный `profile_id`, timestamp window, replay cache, trial decrypt limit,
  late-upgrade `min_records_before_trial` и `upgrade_direction`.
- Allowlist server-side клиентов может задаваться через
  `allowed_client_public_key_files` и/или hex-строки `allowed_client_public_keys`.
- Добавлены unit-тесты config loader-а на client/server v2 configs и негативные
  случаи: missing profile, short key file, mismatched keypair, negative timestamp,
  zero trial limit, invalid direction и empty allowlist.
- `README.md` и `TESTING.md` обновлены: v2 config уже валидируется, но runtime
  relay path пока остается на MVP IFF до следующего wiring-инкремента.

Решения:

- Key files читаются как raw bytes, без PEM/DER и без новой зависимости.
- Public key file обязателен и сверяется с private key, чтобы ловить ошибочную
  раскладку конфигов до запуска relay.
- `security.zero_rtt.profile_id` обязателен при включенном v2, потому channel
  binding должен быть явно привязан к профилю.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=tcp_relay_app --catch_system_errors=no`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`
- `ip netns list | rg 'fps[cs]|fpsprobe' || true`

Commit:

- `8f091c2 Add Zero-RTT relay config parsing`

### Add FPS v2 late upgrade controller

Цель:

- Добавить state-machine слой для позднего zero-RTT upgrade, который отслеживает
  TLS record boundaries с начала потока и сохраняет passthrough fallback.

Сделано:

- Добавлен `FpsUpgradeController`: наблюдает complete TLS records, хранит hash
  предыдущей record и next record index для `ZeroRttChannelBinding`.
- Client-side controller умеет строить TLS application-data upgrade record только
  после появления channel binding.
- Server-side controller trial-decrypt'ит только application-data records в
  configured direction; success strip'ит upgrade record и возвращает `SessionKeys`,
  miss forward'ит bytes как ordinary cover.
- Добавлены unit-тесты на valid late upgrade, ordinary app-data fallback,
  wrong-binding fallback и fragmented TLS boundary handling.
- README/TESTING/CRYPTO_REVIEW обновлены.

Решения:

- Controller пока не интегрирован в `TcpBridgeSession`: MVP IFF path остается
  рабочим baseline до отдельного relay wiring commit.
- Trial-decrypt miss surfaced в result для тестов/метрик, но forwarded bytes
  остаются byte-for-byte.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=fps_upgrade_controller --catch_system_errors=no`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`
- `ip netns list | rg 'fps[cs]|fpsprobe' || true`

### Add encrypted FPS v2 envelope codec

Цель:

- Добавить post-upgrade record-envelope primitive, в котором inner TLS bytes,
  covert frames и metadata шифруются вместе без plaintext sequence/header.

Сделано:

- Добавлен `FpsEnvelopeCodec` с implicit per-direction sequence state для AEAD
  nonce/AAD и wire форматом `ciphertext || tag`.
- Envelope plaintext содержит inner TLS bytes, список covert frames, per-frame
  padding и envelope padding; все структурные поля зашифрованы.
- Добавлены unit-тесты на roundtrip inner TLS + frames + padding, tamper,
  implicit sequence replay failure, oversize/too-many policy и smoke на отсутствие
  plaintext metadata/payload в wire.
- README/TESTING/CRYPTO_REVIEW обновлены с новым v2 primitive.

Решения:

- Envelope codec пока не подключается к relay, чтобы следующий wiring-инкремент
  мог заменить MVP strip-injected-records на уже протестированный core primitive.
- Replay старого envelope без plaintext sequence выглядит как AEAD decrypt failure
  на ожидаемом implicit sequence.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=fps_envelope --catch_system_errors=no`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`
- `ip netns list | rg 'fps[cs]|fpsprobe' || true`

### Add zero-RTT v2 crypto primitives

Цель:

- Начать production refactor с crypto review и минимального проверяемого
  zero-RTT upgrade layer до удаления MVP IFF path.

Сделано:

- Добавлен `CRYPTO_REVIEW.md` с выбранной candidate-конструкцией:
  X25519 + HKDF-SHA256 + ChaCha20-Poly1305 через существующий OpenSSL.
- `crypto` core получил OpenSSL X25519 helpers для public-from-private,
  random keypair и shared-secret derivation.
- Добавлен `ZeroRttUpgradeEngine`: client строит один encrypted upgrade record,
  server trial-decrypt'ит allowlist client static keys с cap, проверяет timestamp,
  replay nonce и channel binding, затем обе стороны получают `SessionKeys`.
- Добавлены unit-тесты на X25519 agreement, valid upgrade, replay, expiry,
  wrong channel binding, wrong client key, malformed candidate, trial limit и tag
  tamper.
- README исправлен на актуальный `FPS_SPEC.md`, TESTING расширен новым baseline.

Решения:

- Новый v2 слой пока живет рядом с MVP IFF, чтобы первый commit был проверяемым
  crypto/core инкрементом.
- Wire upgrade оставляет видимым только ephemeral X25519 public key и ciphertext;
  frame metadata остается внутри AEAD plaintext.
- Channel binding включает direction, record index, previous TLS record hash и
  profile id.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=zero_rtt_upgrade --catch_system_errors=no`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`
- `ip netns list | rg 'fps[cs]|fpsprobe' || true`


### Document post-MVP protocol risks

Цель:

- Зафиксировать в спецификациях спорные архитектурные места, найденные во время
  ревью, чтобы дальнейшая разработка не закрепила MVP wire-format как production
  protocol.

Сделано:

- `research/FPS_SPEC.md` и корневой `FPS_SPEC.md` дополнены разделами про
  active record extraction probing, post-IFF record-envelope mode, late FPS
  authentication, zero-RTT authenticated upgrade candidate, plaintext sequence bug
  и multi-carrier scheduling вместо single primary session.
- `research/SPECS.md` дополнен production-level pitfalls и roadmap items по тем же
  направлениям.
- `README.md` получил короткий раздел `Known Protocol Risks`.

Решения:

- Текущий plaintext sequence prefix явно помечен как protocol/security bug, который
  нужно убрать до adversarial/field tests.
- Zero-RTT authenticated upgrade записан как crypto-review candidate, а не как
  утвержденная конструкция.
- `FPS_SPEC.md` в корне сохранен синхронным с `research/FPS_SPEC.md`.

Проверка:

- `cmp -s FPS_SPEC.md research/FPS_SPEC.md`
- `git diff --check`

### Architecture review of FPS direction

Цель:

- Проанализировать текущую идею FPS, документы `./research`, кодовую архитектуру
  и сравнить подход с Trojan/Cloak и актуальными заметками о ТСПУ/белых списках.

Сделано:

- Прочитаны `AGENTS.md`, `research/FPS_SPEC.md`, корневой `FPS_SPEC.md`,
  `research/README.md`, `research/SPECS.md`, `research/perfect_proxy.md`,
  `research/PITFALLS_ASIO_TLS.md`, `research/TSPU.pdf`, `README.md`,
  `TESTING.md`, `REVIEW.md` и ключевые interfaces/implementations core/relay.
- Подтверждено, что корневой `FPS_SPEC.md` совпадает с `research/FPS_SPEC.md`.
- Сверены внешние референсы: Trojan protocol/docs, Cloak README/wiki и статьи
  Habr про белые списки, Чебурнет 2026 и layered RKN/TSPU diagnostics.
- Подготовлены выводы по маскировке, novelty, расширению за пределы TLS,
  альтернативам и ближайшим рискам устойчивости.

Решения:

- Код и спецификация не менялись: это review-only инкремент.
- Отдельный `REVIEW`-документ не добавлялся, чтобы не дублировать ответ
  пользователю до согласования дальнейшего направления.

Проверка:

- `git status --short`
- `rg --files`
- `cmp -s FPS_SPEC.md research/FPS_SPEC.md`
- `pdftotext research/TSPU.pdf -`

### Add TUN fragmentation for shaper

Цель:

- Разблокировать near-MTU TUN packets под shaper, не переходя к full TLS-flow
  shaping и не меняя IFF bootstrap path.
- Сохранить совместимость старого unfragmented `tun_packet` wire format.

Сделано:

- Добавлен `FrameType::tun_packet_fragment` с payload header:
  `packet_id:u32`, `fragment_index:u16`, `fragment_count:u16`, `total_size:u32`
  в big-endian, затем chunk bytes.
- `TcpBridgeSession` получил batch `enqueue_covert_frames(...)` с preflight
  по суммарному write queue budget, чтобы fragmented packet не оставлял partial
  writes при backpressure.
- `SessionManager` фрагментирует oversized TUN packets на ordered fragment frames,
  reassembles inbound fragments и сбрасывает malformed/out-of-order/mismatched/
  oversized fragment streams без логирования packet bytes.
- `codec.allow_fragmentation` добавлен в JSON config, default `true`; при включенной
  fragmentation `tun.mtu` может быть больше `codec.max_frame_payload`.
- `tun_iff_loopback.py` получил knobs для TUN MTU, codec frame payload и cover
  padding; CTest получил `fps_tun_iff_fragmentation`, а `fps_tun_iff_shaper`
  теперь форсирует fragmentation.
- README/TESTING обновлены для нового контракта и labels.

Решения:

- Fragmentation применяется только к TUN packet payloads; IFF records и ordinary
  cover passthrough остаются immediate.
- Reassembly ordered-only, потому MVP использует одну TCP/TLS-shaped primary
  session без striping.
- Fragmentation пока не negotiated через IFF capabilities; обе стороны должны быть
  одной версии.
- Shaper `next_send_plan(min_payload)` теперь списывает ровно текущий shaped frame,
  а не весь accumulated queued covert budget.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=shaper,covert_codec,session_manager,tcp_relay_app --catch_system_errors=no`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`

Коммит:

- `Add TUN fragmentation for shaper`

### Wire first shaper gate into relay writes

Цель:

- Подключить существующий core `Shaper` к relay write path без поломки cover passthrough,
  IFF bootstrap и real TUN integration.
- Оставить scope узким: шейпить externally injected covert frames, а не весь TLS поток.

Сделано:

- `Shaper::next_send_plan(...)` получил optional `min_covert_payload_size`, чтобы без
  fragmentation не списывать budget, если текущий план не вмещает frame целиком.
- `TcpBridgeSession` получил optional `ShaperProfile`, shaped queue per direction,
  timers на `SendPlan.delay`, учет shaped bytes в существующем `max_write_queue_bytes`
  и payload-safe shaper events.
- Relay JSON config получил `shaper.enabled`, inline profile fields и `shaper.profile_file`
  relative to config path.
- Relay logs теперь пишут `shaper_queued`, `shaper_blocked`, `shaper_scheduled` с
  direction, sizes, delay и queue counters без payload/secrets.
- `tun_iff_loopback.py` получил `--enable-shaper`; CTest получил `fps_tun_iff_shaper`.
- README/TESTING обновлены: shaper больше не marked as future work для injected frames,
  но fragmentation/full cover shaping остаются будущими шагами.

Решения:

- IFF bootstrap records и ordinary cover passthrough остаются immediate в этом инкременте.
- Shaper observes forwarded cover bytes and gates only external `enqueue_covert_frame(...)`
  writes, which currently are TUN/covert injections.
- Пока нет fragmentation/splitting: shaped frame ждет план, где budget >= payload size.
- TUN shaper integration использует isolated netns и не меняет host routes/NAT/firewall.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=shaper,tcp_bridge_session,tcp_relay_app --catch_system_errors=no`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`

Коммит:

- `Wire shaper gate into relay writes`

### Expand regression coverage before shaper

Цель:

- Увеличить default non-sudo regression coverage перед подключением shaper к relay writes.
- Добавить ранние сигналы для CLI stdout/stderr, debug logs, WSS, multi-session IFF,
  config edge cases, shaper state machine и более плотного TUN loopback.

Сделано:

- CTest получил labels `unit`, `local`, `integration`, `log`, `wss`, `tun`, `sudo`
  и timeouts для новых сценариев.
- Добавлены local integration tests: `fps_cli_streams`, `fps_https_iff_logging`,
  `fps_https_iff_multi_session`, `fps_wss_passthrough`, `fps_wss_iff_chain`.
- Python HTTPS harness расширен stdlib-only WebSocket upgrade и masked ping/pong helpers.
- Unit coverage расширен для config loader ошибок, shaper backpressure/profile states,
  explicit primary clear/replace и idempotent TUN pump stop.
- `tun_iff_loopback.py` получил `--udp-count` и `--udp-payload-size`; добавлен
  `fps_tun_iff_burst` на 64 near-MTU UDP echo packets в isolated netns.
- `TESTING.md` и `README.md` обновлены с новой local/TUN matrix.

Решения:

- Режим выбран как All local: ordinary `ctest --test-dir build` запускает максимум
  проверок без root; TUN/root остаются opt-in через `-L tun`.
- Новых внешних зависимостей не добавлялось; WSS реализован на Python stdlib.
- TUN tests по-прежнему не меняют host routes/NAT/firewall и используют isolated netns.

Проверка:

- `python3 -m py_compile tests/integration/*.py`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L local --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -L tun --output-on-failure`
- `git diff --check`

Коммит:

- `Expand regression coverage before shaper`

## 2026-05-09

### Add relay-first logging baseline and self review

Цель:

- Перед shaper добавить приемлемую runtime трассировку без debugger-а.
- Ввести проектную logging facade поверх Boost.Log v2, чтобы будущая замена backend-а
  не требовала переписывать весь код.
- Провести self-review MVP и зафиксировать риски перед shaper.

Сделано:

- Добавлен `fps/log/logging.hpp` и `src/log/logging.cpp`: `Severity`, `LoggingConfig`,
  `parse_log_level(...)`, `init_console_logging(...)` и `FPS_LOG_*` macros.
- CMake теперь подключает Boost.Log v2 и собирает logging implementation в `fps_core`.
- JSON config получил optional `logging.level`; CLI получил `--log-level LEVEL`, который
  override-ит config.
- Runtime relay logs переведены с ad hoc `std::ostream` на `FPS_LOG_*`; `--help` остается
  stdout-only, CLI parse errors остаются в stderr.
- Добавлены relay-first события: startup/listening/stop, accept/connect, per-session
  `session_id`, IFF auth, primary ownership, bridge callback errors, TUN pump/manager errors.
- Добавлен `REVIEW.md` с findings, logging/security notes и shaper-readiness risks.
- `README.md` обновлен с `logging.level`, `--log-level` и payload-safe правилом.
- Добавлены unit-тесты для log level parser, config parsing и CLI override/invalid level.

Решения:

- Первый sink только console, формат text fields.
- Default log level: `info`.
- Не логируются shared secrets, secret file contents, raw IFF material, nonces, session keys,
  raw TLS payloads, raw TUN packets или full IP payload bytes.
- Core crypto/codec/parser пока остаются без прямого логирования; relay трассирует их typed
  errors через существующие callbacks.

Проверка:

- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=logging,tcp_relay_app`
- `./build/fps_client --help > /tmp/fps_help_stdout.txt 2> /tmp/fps_help_stderr.txt`
  -> stdout содержит usage, stderr 0 bytes.
- HTTPS/IFF debug-log smoke с `--log-level debug` -> lifecycle/IFF events найдены,
  `fps-shared-secret` и `secret.bin` в логах не найдены.
- `ctest --test-dir build --output-on-failure`
- `cmake --build cmake-build-tun -j 2`
- `sudo -n ctest --test-dir cmake-build-tun -R 'fps_tun_(open_smoke|iff_loopback)' --output-on-failure`

### Audit regression baseline before shaper

Цель:

- Перед подключением shaper-gated scheduling понять, достаточно ли тестов для быстрого
  обнаружения регрессий в cover TLS, IFF/covert path и real TUN-to-TUN передаче.
- Укрепить главный integration baseline: скрытый TUN traffic не должен ломать живую
  cover TLS session.

Сделано:

- Проведена ревизия текущей карты тестов: ordinary unit/HTTPS tests находятся в `build`,
  real TUN checks вынесены в opt-in `cmake-build-tun`.
- Подтверждено, что `fps_tun_iff_loopback` уже использует два isolated network namespaces,
  два real TUN (`fpsc0`, `fpss0`) и UDP echo через authenticated hidden channel.
- `fps_tun_iff_loopback` усилен: после UDP probe через TUN тот же cover TLS socket теперь
  выполняет второй keep-alive GET. Это проверяет, что injected FPS records были stripped,
  origin-side TLS не сломан, и cover session переживает hidden TUN payload.
- Добавлен `TESTING.md` с regression matrix, командами запуска и осознанными пробелами
  перед shaper.
- `README.md` теперь ссылается на `TESTING.md`.

Решения:

- Real TUN regression остается opt-in из-за `sudo`/`CAP_NET_ADMIN`, но в VM должен
  запускаться перед изменениями shaper/relay scheduling.
- Full routing/NAT-to-Internet, WSS, sustained load/backpressure и fragmentation остаются
  отдельными будущими integration сценариями.

Проверка:

- `python3 -m py_compile tests/integration/tun_iff_loopback.py`
- `sudo -n ctest --test-dir cmake-build-tun -R fps_tun_iff_loopback --output-on-failure`
- `ctest --test-dir build --output-on-failure`
- `sudo -n ctest --test-dir cmake-build-tun -R 'fps_tun_(open_smoke|iff_loopback)' --output-on-failure`

### Bound bridge write queue for TUN backpressure

Цель:

- Убрать unbounded growth на пути `TunPacketPump -> SessionManager -> TcpBridgeSession`,
  если primary TCP session не успевает отправлять injected covert frames.
- Сделать лимит на pending bridge writes явным и проверяемым через JSON config.

Сделано:

- `TcpBridgeSessionConfig` получил `max_write_queue_bytes` с default `1 MiB`.
- `TcpBridgeSession::enqueue_covert_frame(...)` теперь проверяет pending bytes до
  кодирования frame, чтобы отказ `write_queue_full` не сдвигал covert sequence number.
- Pending bytes учитывают queued и in-flight async write items и очищаются при `stop()`.
- `SessionManagerError` получил `write_queue_full`, relay логирует этот случай как
  backpressure rejection для TUN read path.
- JSON config получил optional `limits.max_session_write_queue_bytes`; zero value
  отклоняется config loader-ом.
- `README.md` обновлен с примером и смыслом нового лимита.

Решения:

- Лимит применяется к external covert enqueue; IFF bootstrap records и обычный
  forward path остаются на текущем ordered writer path, потому что real cover reads уже
  возобновляются только после drain forward chunk.
- Default оставлен достаточно большим для существующих integration tests, но bounded
  для saturated TUN/primary-session сценария.
- Shaper-gated scheduling не включался в этом инкременте.

Проверка:

- `cmake --build build -j 2`
- `./build/fps_unit_tests --run_test=tcp_bridge_session,session_manager,tcp_relay_app`
- `cmake --build cmake-build-tun -j 2`
- `ctest --test-dir build --output-on-failure`
- `sudo -n ctest --test-dir cmake-build-tun -R 'fps_tun_(open_smoke|iff_loopback)' --output-on-failure`

### Verify VM TUN/netns baseline

Цель:

- После переноса workspace в виртуальную машину подтвердить, что `sudo`, `/dev/net/tun`
  и network namespaces достаточны для реального TUN-to-TUN integration path.
- Зафиксировать текущую стабильную точку перед следующим архитектурным инкрементом.

Сделано:

- Проверено, что `FPS_SPEC.md` и `research/FPS_SPEC.md` совпадают byte-for-byte.
- Пересобраны обычный `build` и opt-in `cmake-build-tun`.
- Запущены обычные HTTPS/unit tests и оба real TUN smoke tests под `sudo`.

Решения:

- Глобальные routes, firewall/NAT и host networking не менялись; TUN loopback использует
  только isolated network namespaces из test harness.
- Следующий крупный инкремент лучше выбирать между shaper-gated scheduling и
  backpressure/lifecycle hardening, потому что shaper влияет на detectability и контракт
  отправки injected records.

Проверка:

- `cmake --build build -j 2`
- `cmake --build cmake-build-tun -j 2`
- `ctest --test-dir build --output-on-failure`
- `sudo -n ctest --test-dir cmake-build-tun -R 'fps_tun_(open_smoke|iff_loopback)' --output-on-failure`

### Harden sudo TUN smoke coverage

Цель:

- После восстановления `sudo` проверить opt-in TUN tests с root privileges.
- Сделать preflight честным для Docker-сред, где `sudo` есть, но `ip netns add` запрещен mount/capability policy.

Сделано:

- `fps_tun_iff_loopback` теперь заранее пробует временный `ip netns add`; если контейнер запрещает `/run/netns` mount setup, тест возвращает skip 77 вместо failure.
- Добавлен opt-in `fps_tun_open_smoke`: запускает `fps_server --config` с transient TUN, проверяет появление link через `ip link show` и убеждается, что устройство исчезает после остановки процесса.
- `README.md` обновлен: opt-in TUN checks теперь описаны как два уровня smoke, запускать их нужно под `sudo ctest`.

Решения:

- Root-namespace smoke не назначает адреса, routes или NAT; он проверяет только CLI open/lifecycle реального TUN.
- Полноценный netns loopback остается preferred integration test и skip-ается, пока контейнер не разрешает `ip netns add`.

Проверка:

- `python3 -m py_compile tests/integration/tun_open_smoke.py tests/integration/tun_iff_loopback.py`
- `sudo -n ctest --test-dir cmake-build-tun -R 'fps_tun_(open_smoke|iff_loopback)' --output-on-failure` -> `fps_tun_open_smoke` passed, `fps_tun_iff_loopback` skipped 77 из-за запрета `ip netns add`.

### Wire safe real TUN CLI integration

Цель:

- Связать реальный Linux `TunDevice` с CLI relay path без автоматической настройки глобальных routes/NAT/firewall.
- Закрепить ownership primary session, чтобы только первая живая authenticated session писала в TUN.
- Добавить opt-in integration smoke для `/dev/net/tun`, который безопасно пропускается без нужных capabilities.

Сделано:

- JSON config получил optional `tun.enabled/name/mtu/max_write_queue_packets`; `tun.enabled=true` требует `iff.enabled=true`.
- Config loader отклоняет пустой TUN name, zero MTU/queue и `tun.mtu > codec.max_frame_payload`, пока FPS fragmentation не реализована.
- `TcpRelayConfig` хранит роль endpoint-а и optional TUN config; relay открывает TUN, передает fd в `TunPacketPump` через `TunDevice::release_native_handle()` и подключает pump к `SessionManager`.
- `SessionManager` получил primary ownership helpers: `try_set_primary_session`, `is_primary_session`, `clear_primary_session_if`.
- `TcpBridgeSession` handlers в relay назначают first authenticated session primary, очищают stale primary on close и доставляют covert frames в TUN только от primary.
- Добавлен CMake option `FPS_ENABLE_TUN_TESTS=OFF` и opt-in `fps_tun_iff_loopback` Python test с skip code 77 без `/dev/net/tun`, `ip`, OpenSSL или CAP_NET_ADMIN/root.
- `README.md` обновлен с TUN config example, ручной настройкой адресов и командой opt-in теста.

Решения:

- CLI не назначает адреса, routes, forwarding, NAT или MSS clamp; это остается за оператором или isolated test harness.
- При нескольких authenticated sessions первая живая session остается primary; новые authenticated sessions не получают доступ к TUN payload.
- Shaper остается не подключенным к write scheduling в этом инкременте.

Проверка:

- `./build/fps_unit_tests --run_test=tcp_relay_app,session_manager`
- `python3 tests/integration/tun_iff_loopback.py --fps-client ./build/fps_client --fps-server ./build/fps_server` -> skip 77 без CAP_NET_ADMIN/root в текущей среде.
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `cmake -S . -B cmake-build-tun -DCMAKE_BUILD_TYPE=Debug -DFPS_ENABLE_TUN_TESTS=ON`
- `cmake --build cmake-build-tun -j 2`
- `ctest --test-dir cmake-build-tun -R fps_tun_iff_loopback --output-on-failure` -> skipped 77 без CAP_NET_ADMIN/root.


### Add async TUN packet fd pump

Цель:

- Добавить проверяемый fd pump между будущим `TunDevice` и `SessionManager`, не требуя `/dev/net/tun` и не меняя routing.
- Проверить packet-boundary поведение на локальном `socketpair(AF_UNIX, SOCK_DGRAM)`.

Сделано:

- Добавлен `fps::net::TunPacketPump`: владеет fd через Boost.Asio `posix::stream_descriptor`, читает TUN-like packets, вызывает `SessionManager::handle_tun_packet(...)`, и пишет входящие packets обратно в fd через bounded write queue.
- Ошибки pump разделены на closed/empty/oversized/full queue/read/write; ошибки SessionManager от read path пробрасываются отдельным callback.
- Добавлены unit-тесты: exact write в fd, invalid/full queue policy, fd-read -> primary session -> `TUN_PACKET` covert frame, no-primary read errors без остановки pump, и `SessionManager` sink -> fd write.
- Real CLI wiring к `/dev/net/tun`, настройка адреса/MTU/routes/NAT и shaper gating не включались.
- `README.md` обновлен с текущим статусом async packet fd pump.

Решения:

- Pump принимает ownership fd; будущая интеграция с `TunDevice` должна передавать released/duplicated descriptor без double-close.
- Для тестов используется datagram socketpair, потому что он сохраняет границы packet-like сообщений лучше обычного pipe.
- При отсутствии primary session pump не падает и продолжает читать, но событие фиксируется callback; pause/drop policy останется для backpressure-инкремента.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`

### Add TUN packet SessionManager plumbing

Цель:

- Связать authenticated primary session с TUN packet payload без поднятия real TUN/routing.
- Проверить, что opaque IP packet bytes проходят через `TUN_PACKET` covert frames между FPS endpoints.

Сделано:

- Добавлен `fps::net::SessionManager`: role-based mapping client/server, weak primary session, `handle_tun_packet(...)`, `handle_covert_frame(...)` и sink callback для будущей записи в `TunDevice`.
- Политики MVP-инкремента: нет unbounded queue, empty/oversized packets отклоняются typed errors, отсутствие primary session возвращает `no_primary_session`, non-TUN и wrong-direction frames игнорируются через event callback.
- Добавлены unit-тесты направления отправки, no-primary, empty/oversized policy, incoming sink delivery и ignored frame events.
- Добавлен in-process loopback test: client/server `TcpBridgeSession` проходят valid IFF, затем fake client/server TUN packets ходят в обе стороны и не попадают в browser/origin sockets.
- Real `/dev/net/tun`, network namespaces, routes, NAT, MTU/MSS и shaper-gated scheduling оставлены следующим инкрементом.
- `README.md` обновлен с текущим статусом fake-TUN plumbing.

Решения:

- `SessionManager` не владеет transport lifecycle; он хранит только `weak_ptr<TcpBridgeSession>`.
- TUN packets пока opaque bytes без IPv4 validation, fragmentation/reassembly и MTU policy.
- Default `max_tun_packet_size` зафиксирован как `1500` для seam-level тестов, не как финальный route MTU.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`

### Expand HTTPS integration regressions

Цель:

- Закрепить multi-roundtrip browser-origin traffic до TUN/shaper.
- Добавить negative IFF integration test с controlled TLS failure и без падения FPS процессов.

Сделано:

- Вынесен общий Python harness `fps_https_harness.py`: self-signed cert, HTTPS origin, process lifecycle, config writer, HTTP/1.1 response parsing.
- `https_passthrough.py` и `https_iff_chain.py` теперь открывают один TLS socket и выполняют три GET round trips: `/round/0`, `/round/1`, `/round/2`.
- Origin работает в HTTP/1.1 keep-alive mode и записывает порядок request paths; tests проверяют тела ответов и порядок paths.
- Добавлен `https_bad_iff.py`: client/server используют разные secrets, browser TLS/HTTP ожидаемо fails, origin не получает HTTP request, `fps_client` и `fps_server` остаются живы до teardown.
- CTest теперь включает `fps_https_bad_iff`.
- `README.md` обновлен с direct командой bad-IFF smoke.

Решения:

- Bad IFF закреплен как controlled TLS/session failure, а не process crash.
- Текущая политика mismatch остается прежней: invalid IFF bytes идут по fallback path и ломают TLS у origin.
- Concurrent sessions остаются следующим отдельным integration-инкрементом.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`

### Add relay JSON config and CLI IFF chain

Цель:

- Включить IFF из публичных `fps_client`/`fps_server`, а не только через programmatic `TcpBridgeIffOptions`.
- Закрепить end-to-end сценарий `fps_client --config -> fps_server --config -> HTTPS origin`.

Сделано:

- Добавлен `load_tcp_relay_config(...)` для JSON config с секциями `network`, `iff`, `codec`.
- CLI получил `--config PATH`; flags `--listen`, `--origin`/`--server`, `--target`, `--read-buffer` остаются для passthrough/debug.
- `TcpRelayConfig` теперь может нести `TcpBridgeIffOptions`; `TcpRelayServer` передает их в каждую новую `TcpBridgeSession`.
- `iff.secret_file` читается как raw bytes; relative path разрешается относительно config file.
- Поддержаны IFF параметры `timestamp_window_sec`, `version`, `capabilities`, `max_padding_size`, `auto_start_client`, padding/nonce hex для deterministic tests, а также codec limits.
- Добавлен Python integration smoke `https_iff_chain.py`: поднимает HTTPS origin, `fps_server --config`, `fps_client --config`, затем делает TLS GET через client relay с включенным IFF.
- `README.md` обновлен с примером JSON config и командами integration smoke.

Решения:

- Для MVP config parser использует Boost.PropertyTree, чтобы не добавлять новые внешние зависимости.
- `secret_file` не trim-ится: содержимое файла является точным shared secret.
- Deterministic nonce/padding через hex оставлены для тестов; production config должен их не задавать, чтобы nonce генерировались случайно.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`

## 2026-05-08

### Wire IFF into TCP bridge

Цель:

- Подключить `IffSessionController` к живому `TcpBridgeSession`, не меняя пока публичный CLI/config.
- После успешного IFF автоматически активировать `CoverSessionPipeline` с derived session keys.

Сделано:

- Добавлен `TcpBridgeIffOptions` в `TcpBridgeSessionConfig`: IFF controller config, timestamp provider, client nonce/padding, auto-start для client role и codec payload/padding limits.
- `TcpBridgeSession` теперь умеет pre-auth обработку только peer-side направления: client role читает IFF с `server_to_client`, server role - с `client_to_server`.
- Client role автоматически enqueue-ит TLS-record-wrapped IFF ClientHello при `start()`, если включен `auto_start_client`.
- Server role strip-ит valid ClientHello, enqueue-ит ServerHello в обратном направлении, затем strip-ит Finish и получает session keys.
- После authentication bridge пересобирает role-aware pipelines: peer-side direction получает `CovertCodec`, browser/origin-side direction остается passthrough во избежание false positives на настоящем TLS.
- Добавлены loopback-тесты на server-side IFF handshake + последующий covert decode, client-side auto-start + stripping ServerHello/Finish, и сохранение partial TLS record на real origin/browser стороне при активации auth pipelines.
- `README.md` уточняет, что IFF wiring пока programmatic; публичный CLI еще не загружает secret/config и не включает IFF.

Решения:

- IFF включается только на стороне FPS peer socket. Реальный browser/origin поток остается passthrough даже после auth.
- При активации auth меняются только peer-side pipelines; противоположный passthrough pipeline не заменяется, чтобы не терять уже накопленные partial TLS bytes.
- `TcpBridgeSession` пока не занимается shaper scheduling для IFF records: записи идут через общую ordered write queue, а временной профиль будет отдельным следующим слоем.
- На этом шаге не добавлялся config loader; secrets в тестах задаются напрямую через `TcpBridgeIffOptions`.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`

### Add IFF session controller and HTTPS integration smoke

Цель:

- Закрепить no-IFF HTTPS passthrough как воспроизводимый integration test.
- Добавить тестируемый IFF/session слой без сокетов перед вмешательством в живой `TcpBridgeSession`.

Сделано:

- Добавлен `tests/integration/https_passthrough.py`: генерирует self-signed cert, поднимает Python HTTPS origin, запускает `fps_server` relay и делает TLS GET через relay.
- CTest теперь включает `fps_https_passthrough`, если найден `Python3::Interpreter`.
- Добавлен `IffSessionController`: client/server роли, states `cover_passthrough`, `iff_candidate`, `authenticated_primary`, TLS-record-wrapped IFF ClientHello/ServerHello/Finish и выдача `SessionKeys`.
- На невалидных или нераспознанных IFF candidates controller оставляет bytes в passthrough, чтобы fallback не отличался от обычной TLS-сессии.
- Добавлены unit-тесты на успешный client/server handshake, no-IFF passthrough, bad secret fallback, fragmented ClientHello, transcript mismatch и invalid role.
- `README.md` обновлен с direct командой integration smoke.

Решения:

- Новый controller сознательно останавливается на handshake/authenticated state: authenticated data phase должен переключиться на `CoverSessionPipeline` с `CovertCodec`, а не жить внутри IFF слоя.
- IFF ClientHello/ServerHello/Finish пока оборачиваются в TLS application-data records напрямую через `TlsRecordLayer`; shaper scheduling будет добавлен поверх этого outbound path следующим слоем.
- Неудачная проверка IFF не логируется как ошибка в result для ordinary fallback path, чтобы random TLS application-data не создавал шум и не менял поведение.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`

### Add runnable TLS relay CLI

Цель:

- Сделать первый запускаемый Linux CLI для ручной проверки сквозного TLS passthrough поверх `TcpBridgeSession`.
- Исключить ложное stripping настоящих TLS application-data records до появления полноценного IFF/session state.

Сделано:

- Добавлен явный `CoverSessionPipeline::passthrough()`: он парсит TLS records и пересобирает forwarded bytes, но никогда не пытается decode/strip FPS candidates.
- Добавлен `fps::net::tcp_relay_app`: parser `HOST:PORT`, общий `run_tcp_relay(...)`, CLI helper, async accept/resolve/connect и создание `TcpBridgeSession` с passthrough pipelines.
- `fps_server` теперь запускается как `--listen HOST:PORT --origin HOST:PORT`; `fps_client` как `--listen HOST:PORT --server HOST:PORT`; общий alias `--target` тоже поддержан.
- Добавлены unit-тесты на endpoint parsing и passthrough application-data с payload, похожим на начальный sequence.
- `README.md` обновлен с командами текущего relay CLI и явным ограничением: это пока TLS-record relay, не IFF/covert/TUN tunnel.

Решения:

- CLI использует passthrough pipelines, чтобы ordinary TLS не зависел от эвристики `next_receive_sequence`.
- Relay пока предназначен для TLS-record-shaped потоков; arbitrary TCP bytes не являются целью этого слоя, потому что bridge уже работает на уровне TLS records.
- Полноценные Python HTTPS/WSS integration tests стоит оформить отдельным тестовым harness, а не встраивать в unit suite.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `./build/fps_client --help`
- `./build/fps_server --help`
- Python HTTPS GET smoke через `fps_server --listen 127.0.0.1:18443 --origin 127.0.0.1:19443`

### Add TCP bridge injection queue

Цель:

- Добавить безопасный single-writer путь для injected covert frames в `TcpBridgeSession`, чтобы будущий TUN reader/shaper мог писать FPS records в выбранном направлении.

Сделано:

- Добавлен `TcpBridgeSessionPipelines` с отдельными inbound/outbound pipelines для C2S и S2C, потому что decode- и encode-направления codec не всегда совпадают в одном endpoint.
- Добавлен `enqueue_covert_frame(...)`, который кодирует covert frame, оборачивает его в TLS application-data record и ставит в per-direction write queue.
- Forwarded real TLS bytes и injected records теперь проходят через одну очередь на направление, без параллельных writes в один socket.
- TCP half-close теперь выполняет `shutdown_send` только после drain очереди соответствующего направления.
- Добавлены loopback-тесты на injection S2C, ordering injected+forwarded bytes и отказ enqueue после `stop()`.

Решения:

- Старый `create(...)` с двумя pipelines оставлен для совместимости; новый overload принимает четыре pipelines явно.
- Public enqueue рассчитан на вызов из того же Asio execution context; cross-thread posting будет отдельным слоем, если понадобится.
- Forward path по-прежнему возобновляет read только после записи forward chunk, сохраняя естественный backpressure.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`

### Add TCP bridge session skeleton

Цель:

- Добавить первый Boost.Asio TCP passthrough слой поверх `CoverSessionPipeline`, пока без TUN routing, IFF state machine и shaper-driven injection queue.

Сделано:

- Добавлен `fps::net::TcpBridgeSession`: две async read/write цепочки, C2S/S2C pipelines, callbacks для covert frames и parse/codec/record errors.
- Реальные TLS records прокидываются byte-for-byte в целевой socket; FPS records извлекаются через pipeline и не доходят до другой стороны.
- Настоящий TCP EOF транслируется как `shutdown_send` на противоположный socket; hard errors закрывают session.
- Добавлены loopback-тесты на `127.0.0.1`: client->origin passthrough, origin->client passthrough, covert stripping и tampered candidate error.
- CMake теперь линкует Boost.System для Boost.Asio socket слоя.

Решения:

- В этом инкременте нет write queues и внешнего injection API: одна read/write цепочка на направление дает естественный backpressure и проще проверяется.
- `connection_reset` считается hard error, а не graceful EOF.
- Тесты остаются без root/TUN и не требуют внешнего сервера.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`

### Add cover session byte pipeline

Цель:

- Собрать первый безсокетный session core, который соединяет TLS parser, TLS record layer и CovertCodec.

Сделано:

- Добавлен `CoverSessionPipeline`: encode covert frame -> TLS application-data record, process inbound TLS byte chunks -> forwarded real TLS bytes + decoded covert frames.
- Pipeline буферизует partial TLS records через `TlsRecordParser`.
- Application-data record считается FPS-кандидатом только если его payload начинается с ожидаемого sequence number; иначе record идет в passthrough.
- Если sequence совпал, но AEAD/codec validation упали, record снимается как подозрительная FPS-запись и ошибка попадает в `codec_errors`.
- Добавлены unit-тесты на оба направления, обычный TLS passthrough, non-application-data passthrough, partial records, tampered candidate, oversized payload и coalesced real/covert stream.

Решения:

- В этом инкременте `Shaper` еще не участвует: pipeline отвечает только за корректную byte-stream классификацию и wrapping/stripping.
- TCP lifecycle, half-close, backpressure и socket queues остаются следующим слоем поверх `CoverSessionPipeline`.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`

### Add TLS record injection layer

Цель:

- Добавить core-слой, который оборачивает opaque FPS bytes в TLS application-data records и фильтрует распарсенные TLS records для будущего session pipeline.

Сделано:

- Добавлен `TlsRecordLayer`: builder TLS application-data records, classifier-based filter, helpers для подсчета/склейки forwarded records.
- Добавлены unit-тесты на построение record, oversized payload, passthrough non-application-data, strip/extract covert payload, работу с coalesced parser output и malformed records.
- В `README.md` зафиксирована будущая идея интеграционных тестов: Python TLS/WSS client-server harness с самоподписанным сертификатом, HTTPS GET и WSS ping/pong сценариями.

Решения:

- Новый слой фильтрует уже распарсенные `TlsRecord`; TCP stream lifecycle и неполные байты останутся ответственностью следующего session pipeline.
- Classifier callback вызывается только для TLS application-data records. Handshake/alert/CCS records всегда идут в passthrough.
- В этом инкременте classifier абстрактный; следующая сборка session pipeline сможет подключить туда IFF/CovertCodec detection.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`

### Add CovertCodec and IFF core

Цель:

- Реализовать тестируемый криптографический frame layer без сетевого session pipeline.
- Зафиксировать MVP IFF как HMAC-proof candidate без plaintext magic marker.

Сделано:

- Добавлены OpenSSL/libcrypto helpers: random bytes, HMAC-SHA256, SHA-256, HKDF-SHA256 и ChaCha20-Poly1305 AEAD.
- Добавлен `IffEngine`: client hello, server hello, finish verification, timestamp window, replay cache и session key derivation.
- Добавлен `CovertCodec`: AEAD session frames с plaintext `seq_be64`, encrypted frame header/payload/padding, direction-separated keys, replay/old-sequence rejection.
- Добавлены typed errors и общий `Result<T, Error>` helper.
- CMake теперь линкует `fps_core` с `OpenSSL::Crypto`.
- Добавлены unit-тесты на валидный IFF, bad secret, expiry, replay, transcript mismatch, AEAD roundtrip, tamper, wrong direction, replay, padding и oversized payload/padding.

Решения:

- IFF domain labels используются только внутри HMAC/HKDF input и не появляются на проводе.
- Первый codec-инкремент отклоняет oversized payload; fragmentation/reassembly остается следующим шагом перед TUN hookup.
- CovertCodec предполагает ordered stream: ожидается точный следующий sequence number; старый sequence считается replay, будущий sequence - invalid wire.

Проверка:

- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`
- `./build/fps_client`
- `./build/fps_server`

### Start Linux implementation scaffold

Цель:

- Начать реализацию Linux-only MVP так, чтобы core оставался переиспользуемым для будущего Android-приложения.
- Получить первый проверяемый C++20/CMake инкремент без TUN/root integration.

Сделано:

- Добавлен CMake-проект с библиотекой `fps_core`, placeholder executables `fps_client` и `fps_server`, Boost.Test unit tests.
- Добавлены базовые core-типы `Direction`, `Priority`, `ByteVector`.
- Реализован incremental `TlsRecordParser` для TLS record headers, partial/coalesced records, oversized records и resync после неверного header.
- Реализован static profile-driven `Shaper` с deterministic seed, CDF sampling, covert budget, burst limit и backpressure handling.
- Добавлена Linux TUN RAII-обертка `fps::linux_platform::TunDevice` вокруг `/dev/net/tun` и `TUNSETIFF`.
- Добавлены `.gitignore` и корневой `README.md` с командами сборки/тестов.

Решения:

- Первый инкремент не реализует session pipeline, codec/IFF и реальный TCP passthrough; он создает тестируемый фундамент для них.
- TUN integration tests будут отдельными от unit-тестов, потому что требуют root/capabilities и сетевой настройки.
- Внешние зависимости не добавлялись: хватило установленного `g++ 13.3`, `cmake 3.28` и Boost 1.83.

Проверка:

- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`
- `cmake --build build -j 2`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`
- `./build/fps_client`
- `./build/fps_server`

### Bootstrap project guidance

Цель:

- Создать корневой `AGENTS.md` с правилами для будущей реализации FPS.
- Зафиксировать рабочую дисциплину: Git, регулярные коммиты, проверка истории, диалог с пользователем, тестирование, C++ best practices, sudo/Docker safety и журнал работ.

Сделано:

- Добавлен `AGENTS.md`.
- Добавлен этот `WORKLOG.md` как стартовый журнал.
- Инициализирован Git-репозиторий в корне workspace.
- Настроена локальная Git-идентичность `Codex <codex@local>` для коммитов агента.

Решения:

- Git должен использоваться как источник правды для истории изменений.
- Основная спецификация MVP указана как `research/FPS_SPEC.md`; при наличии корневого `FPS_SPEC.md` агент обязан сравнить документы перед реализацией.
- Предпочтительный C++ baseline: `g++`, C++20, Boost.Asio, Boost.Test, Boost.JSON/Log/ProgramOptions при необходимости.

Проверка:

- Проверено наличие файлов.
- Выполнить первый Git-коммит текущего состояния workspace.
