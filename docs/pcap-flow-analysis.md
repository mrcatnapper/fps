# FPS TCP Flow Shape Experiment

This page records an exploratory pcap analysis of the visible
`fps_client <-> fps_server` TCP flow before and after Zero-RTT upgrade. It is
not a regression gate and it is not a censorship-evasion proof. The goal is to
measure whether the current beta implementation changes packet-size and timing
distributions in a way that is visible below TLS.

## Scenario

The experiment used a local Docker/TUN topology:

- one `fps_server`;
- one `fps_client`;
- one debug HTTPS/WSS `fps_carrier` origin;
- one persistent WSS carrier client;
- bidirectional UDP `iperf3` traffic over the FPS TUN lease.

The carrier generator produced continuous WSS echo traffic. Zero-RTT upgrade was
intentionally delayed with `--pre-upgrade-records 60` so the capture had a small
pre-upgrade window and a longer post-upgrade window. The pcap was captured on
the concrete Docker bridge interface for the FPS TCP link. Capturing Docker
traffic on Linux `any` can duplicate or reorder packets enough to confuse TCP
reassembly and TLS record checks.

Reproduction command:

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

Readable plots were generated from the analyzer CSV/JSON with an optional local
Python venv:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install matplotlib pandas numpy
.venv/bin/python tools/plot_pcap_flow.py \
  --packets-csv captures/<project>/flow-packets.csv \
  --summary-json captures/<project>/flow-summary.json \
  --out-prefix captures/<project>/readable-flow
```

The plotting dependencies are developer-only analysis tools. They are not FPS
runtime, Docker image, CMake or CI dependencies.

## Wire-Shape Check

The captured FPS link still parsed as TLS records in both directions:

```text
total TLS records: 14697
client -> server Application Data records: 7346
server -> client Application Data records: 7347
content types observed: ChangeCipherSpec, Handshake, ApplicationData
```

This confirms only syntactic TLS record framing. It does not say that the flow's
packet sizes and timing still match the carrier's original distribution.

## Plots

![Visible FPS TCP-link packet size and timing overview](./assets/pcap-flow/bidir-overview.png)

![Packet-size and inter-packet quantiles by direction](./assets/pcap-flow/bidir-quantiles.png)

## Measured Result

The bidirectional UDP probe completed without packet loss:

| Direction | UDP target | Observed UDP throughput | Lost packets |
| --- | ---: | ---: | ---: |
| client to server | 2 Mbit/s | about 2.00 Mbit/s | 0 |
| server to client | 2 Mbit/s | about 2.00 Mbit/s | 0 |

Visible FPS TCP-link packet statistics:

| Phase | Duration | Packets | IP throughput | IP size p50 | IP size p95 | inter-packet p50 | inter-packet p95 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| before upgrade | about 2.02 s | 268 | about 5.03 Mbit/s | 119 B | 11195 B | 0.19 ms | 33 ms |
| after upgrade | about 33.50 s | 17234 | about 8.84 Mbit/s | 1321 B | 10188 B | 0.82 ms | 4.28 ms |

After upgrade, both TCP directions carried roughly 4.4 Mbit/s of visible IP
traffic. The median packet size in both directions moved to approximately
1321 bytes, while the upper tail still contained large WSS/TLS records around
9-11 KiB.

## Interpretation

The current implementation preserves TLS record syntax: Wireshark and the
libpcap checker can still parse the FPS link as TLS. However, the packet-size
and timing distributions visibly change after FPS starts carrying TUN traffic.

The strongest signal in this experiment is not an FPS plaintext marker or a
broken TLS record boundary. It is a traffic-shape signal:

- a new dense band of near-MTU packets appears after upgrade;
- inter-packet timing becomes much more frequent than the pre-upgrade carrier
  cadence;
- bidirectional TUN load makes this effect visible in both directions.

This is expected for the current beta. FPS currently prioritizes correctness,
lease enforcement, classified-record confidentiality and Docker-first operability over
traffic-shape mimicry.

## Engineering Conclusion

The next traffic-analysis work should focus on classified-record scheduling and shaping,
not on TLS record syntactic validity alone. Useful next experiments:

- compare a pure carrier capture against carrier-plus-FPS captures at several
  TUN rates;
- cap or pace covert datagram frames so they do not create a separate near-MTU packet
  band;
- schedule FPS envelopes against the carrier's observed cadence instead of
  flushing immediately whenever TUN data is available;
- rerun the same pcap analysis over real browser/application carrier sessions,
  not only the deterministic debug carrier;
- keep `is_pcap_looks_like_tls.py` as a basic wire-shape regression, but add a
  separate research workflow for packet-size/timing distributions.

For now, treat these plots as a diagnostic baseline. They show that FPS is
syntactically TLS-shaped, but not yet statistically shaped.
