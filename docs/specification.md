# FPS Protocol And Architecture Specification

Version: 0.11, transcript-bound classified-record transport and adaptive shaper
beta snapshot

Implementation language: C++20, Boost.Asio, Boost.Test, Boost.JSON, Boost.Log
and OpenSSL.

## 1. Purpose

FPS is an experimental covert datagram transport carried over live TLS cover
sessions. The current product adapter, and the first production-facing use case,
maps those datagrams to a leased L3 TUN VPN service.
On the `fps_client <-> fps_server` link an observer should see a TCP stream
made of TLS Application Data records. Real browser/origin TLS bytes are not
terminated by FPS. After upgrade, ordinary carrier TLS records continue to be
forwarded byte-for-byte; FPS inserts separate classified TLS Application Data
records for opaque datagram/control payloads and consumes them before they
reach the real TLS endpoints.

The expanded project name, Free Porn Storage, is a deliberate misdirection. It
is not descriptive branding; it is meant to make casual discovery and search by
unprepared users or classifiers less useful. The protocol and implementation
documents should still use the short name, FPS, for technical clarity.

Linux is the active target platform for both client and server. Android client
work has started through a command-line Kotlin/NDK scaffold; the current build
separates protocol core, carrier/datagram core, TUN adapter and Linux runtime
targets so future adapters can reuse authenticated carriers without inheriting
Linux TUN or `ip` orchestration.

## 2. Architecture Baseline

```text
Browser / cover client
        |                                            server TUN
        v                                                |
fps_client == TLS-application-record-shaped FPS link == fps_server
        |                                                |
   client TUN                                            v
                                                   real HTTPS origin
```

Key v5 properties:

- `security.zero_rtt` is the only supported carrier authentication mechanism.
- After a successful Zero-RTT upgrade, ordinary carrier TLS records remain
  byte-for-byte visible on the FPS link.
- FPS records are inserted as separate TLS Application Data records with
  transcript-bound hints, AEAD-encrypted opaque datagram/control frames and
  padding. Frame metadata is not visible at plaintext offsets.
- Authenticated sessions do not downgrade. Until TCP is closed by the browser,
  origin or FPS endpoint, the session stays authenticated even when no covert
  payload is available.
- Opaque datagrams are scheduled through a carrier pool. The Linux TUN adapter
  maps IP packets to those datagrams; any authenticated Zero-RTT TLS session can
  carry them.
- There is no primary-session ownership model in the target architecture.

Implementation boundary:

- `fps_protocol_core` owns protocol primitives: TLS record parsing/wrapping,
  transcript-bound Zero-RTT, classified FPS records, envelope/frame codecs and
  shaping decisions. It does not open sockets or TUN devices.
- `fps_datagram_core` owns the generic unreliable datagram transport contract:
  `CovertDatagramTransport` schedules opaque datagram frames across abstract
  `CovertCarrier` handles identified by `CarrierId`. This layer does not assume
  that the carrier is TLS, TCP, SSH, WebRTC or any other concrete protocol.
- `fps_tls_tcp_carrier` owns the current TLS-over-TCP carrier implementation.
- `TlsTcpCarrierSession` is the current concrete carrier implementation. It is
  deliberately named TLS/TCP because it owns TCP socket reads/writes, TLS record
  slicing, carrier transcript tracking, Zero-RTT state transitions and
  insertion/removal of classified FPS TLS Application Data records.
- `TunTunnelAdapter` is the first product adapter above the datagram transport:
  it maps leased IPv4 TUN packets to opaque datagrams and enforces server-side
  lease/source/destination routing. Future adapters can target the same
  datagram contract without inheriting TUN semantics.
- `fps_linux_runtime` composes the production Linux relay, config/CLI, status
  socket, Linux TUN runtime and operator-facing daemon behavior.

Carrier origin resolution is an operator concern. For browser-created carrier
sessions in beta deployments, the recommended client-side mechanism is a
minimal `/etc/hosts` override that maps the carrier hostname to the local
`fps_client` listener while keeping the browser-visible hostname, SNI and
`Host` header unchanged. FPS does not currently include a DNS proxy or global
resolver helper.

## 3. Wire Rules

Every byte added by FPS on the `fps_client <-> fps_server` link must be wrapped
in an outer TLS record:

```text
+------------+-------------+-------------+------------------+
| type = 23  | version     | length      | opaque bytes     |
+------------+-------------+-------------+------------------+
| 1 byte     | 2 bytes     | 2 bytes     | length bytes     |
+------------+-------------+-------------+------------------+
```

Rules:

- Outer content type must be TLS Application Data (`23`).
- FPS must not send plaintext FPS frame headers directly on top of TCP.
- Real browser/origin TLS endpoints must never receive FPS upgrade or classified
  records.
- Before upgrade, unrecognized records are proxied byte-for-byte.
- After upgrade, ordinary carrier records are forwarded and transcript-tracked.
  A classified-record hint miss is treated as carrier; a hint match with failed
  decrypt/validation closes or drains the FPS session.
- Removing, inserting or changing a visible FPS record breaks the FPS carrier;
  modifying ordinary carrier TLS records remains a normal TLS integrity failure
  at the browser/origin endpoints.

Wire-shape checks:

- local tests cover passthrough and v5 classified-record paths;
- manual capture uses `tools/capture_tls_wire.sh --port PORT -- COMMAND`;
- when `tshark` is installed, the generated TLS summary should show parseable
  TLS records rather than Wireshark falling back to an opaque TCP stream.

## 4. Zero-RTT Authentication

`ZeroRttUpgradeEngine` uses X25519, HKDF-SHA256 and ChaCha20-Poly1305 through
OpenSSL.

Current construction:

- A client is represented in user-facing config only by `client_uuid`; FPS
  deterministically derives the client's X25519 key pair from that UUID.
- The server has a static X25519 key pair in inline base64 config and an
  allowlist of client UUIDs.
- `client_uuid` is a per-device/per-profile bearer secret. Shared or group UUIDs
  are unsupported because one UUID maps to one client public key and one
  persistent TUN lease identity.
- The client builds one encrypted auth record only after observing TLS
  Application Data in both carrier directions, enough real carrier TLS records
  to form a bidirectional transcript binding and the configured client-side
  upgrade delay. The default client delay is two seconds. It lets a fresh relay
  observe initial carrier record-size/timing behavior before inserting FPS
  records.
- Upgrade associated data binds the attempt to the bidirectional TLS record
  indices, transcript byte counts, transcript hashes and profile id.
- The transcript is an incremental cryptographic hash of visible carrier TLS
  record bytes before the candidate record, seeded with public
  domain-separated material: server public key, profile id, direction and
  protocol version.
- The auth record starts with two short opaque hints, not with a public-key
  shaped field. The rest of the capsule is encrypted.
- Timestamp and replay-cache fields are not part of the active v5 wire format.
  Replay resistance relies on reproducing the exact bidirectional carrier
  transcript prefix and completing the server accept leg. Reintroducing durable
  replay state remains a possible protocol-review outcome.
- The server sends a distinct encrypted accept record after authenticating the
  client. Final classified-record keys are derived only after this accept leg.
  The accept payload can carry encrypted control metadata such as the
  server-assigned TUN lease.

Security constraints:

- Do not log private keys, raw upgrade plaintext, nonces, session keys, raw TLS
  payloads, raw TUN packets or IP payload bytes.
- Client config contains `client_uuid` and `server_public_key_base64`.
- Server config contains `server_private_key_base64`,
  `server_public_key_base64` and `allowed_client_uuids`.
- An invalid candidate before upgrade must look like an ordinary passthrough
  miss.
- A classified-record hint match followed by invalid decrypt/validation after
  upgrade is a session failure.

### 4.1 Transcript-Bound Precheck

Implemented v5 client-auth precheck layout inside the TLS Application Data
payload:

```text
server_hint[8] | client_hint[8] | encrypted_capsule | capsule_tag[16]
```

Hints are derived from the pre-candidate transcript snapshot:

```text
server_hint = H("fps/zero-rtt/client-auth/server-hint/v5" ||
                bidirectional_transcript_snapshot ||
                server_public_key || profile_id)[0..8]

client_hint = H("fps/zero-rtt/client-auth/client-hint/v5" ||
                bidirectional_transcript_snapshot || client_public_key ||
                server_public_key || profile_id)[0..8]
```

Server-side verification first rejects candidates with a wrong `server_hint`,
then scans the configured UUID-derived client public keys for `client_hint`, and
only then attempts capsule decryption for the likely client. The encrypted
client-auth capsule contains protocol version, capabilities, the client
ephemeral public key and opaque client payload.

The server accept record uses the same visible shape with server-accept labels:

```text
server_hint[8] | client_hint[8] | encrypted_accept_capsule | accept_tag[16]
```

The encrypted accept capsule contains protocol version, capabilities, the
server ephemeral public key and opaque server payload. Session keys are derived
from static-static DH, client-ephemeral/server-static DH,
server-ephemeral/client-static DH, ephemeral-ephemeral DH, the bidirectional
transcript snapshot and both encrypted auth/accept wire records.

This is not a complete active CPU DoS defense because an attacker that knows the
public construction can create plausible `server_hint` values. Its main value is
removing visible public-key-shaped handshake material and avoiding full decrypt
work for ordinary random carrier records.

### 4.2 Server Accept Race Handling

The server accept record is not guaranteed to be the very next TLS record
observed by the client after it sends the Zero-RTT auth record. Browser and
origin TCP streams are independent, and ordinary origin-to-browser TLS records
can race ahead of the FPS server's accept record.

Required client behavior: after sending an auth candidate, the client must
trial-decrypt plausible peer-direction TLS Application Data records as possible
server accepts. If a record does not classify as a valid accept record, the
client must forward it
byte-for-byte to the browser as cover traffic and keep waiting until the
accept arrives or a bounded policy expires. Only after a valid accept does the
client treat the carrier as authenticated. Treating the first peer-direction
record as mandatory accept is a correctness bug and can
break otherwise valid cover sessions.

### 4.3 Future No-Bootstrap Classifier

The same transcript-bound idea can become a later no-bootstrap FPS record
classifier: every visible TLS Application Data record could be tested as either
ordinary carrier bytes or an FPS candidate without a prior explicit
authentication record. This is a larger future design, not the beta baseline. It
must prove:

- extremely low false positives, because swallowing a real carrier TLS record
  would break the browser/origin TLS stream;
- extremely low false negatives, because forwarding a real FPS record to the
  browser/origin would also break the stream;
- deterministic transcript-state agreement across both FPS peers despite TCP
  direction races;
- safe nonce/key derivation without an explicit upgrade state;
- clear behavior for idle periods where FPS wants to send data while the carrier
  application has no bytes to forward.

## 5. Classified FPS Records

After upgrade, FPS no longer wraps the entire carrier TLS byte stream. Each
ordinary carrier TLS record is forwarded byte-for-byte and included in the
per-direction transcript. Covert traffic is inserted as a separate TLS
Application Data record:

```text
server_hint[8] | client_hint[8] | encrypted_record | record_tag[16]
```

Hints are derived from direction, profile id, authenticated client/server public
keys, session keys, the per-direction carrier transcript snapshot, visible
record index and implicit FPS sequence. The AEAD associated data binds the same
metadata plus the visible payload length.

Encrypted plaintext contains protocol version, flags, implicit sequence, an
opaque datagram/control frame bundle and padding. It does not contain real
carrier TLS bytes.

Sequence and nonce discipline:

- sequence is implicit per direction;
- nonce is built from per-direction session material and implicit sequence;
- sequence, frame count, frame type, payload length and padding length are
  inside AEAD plaintext. On wire, only hints, ciphertext and tag are visible
  inside the TLS Application Data record.
- hint miss means ordinary carrier record and is forwarded; hint match with
  failed AEAD/sequence/version/frame validation closes the carrier.

## 6. Covert Datagram Transport And TUN Adapter

The protocol core exposes a best-effort opaque datagram transport over
authenticated carrier sessions. The transport does not know whether the payload
is an IP packet, a control message for a higher-level adapter or a future
application-specific payload.

`CovertDatagramTransport` maintains the generic carrier pool:

- generic `CovertCarrier` handles are registered only after Zero-RTT
  authentication;
- carrier enqueue is synchronous and same-executor: callers must submit
  datagrams from the carrier owner executor; implementations may reject calls
  from other threads with `wrong_executor`;
- outbound opaque datagrams select carriers round-robin while respecting
  closed/full queues;
- if one carrier write queue is full, the transport tries another carrier;
- if no carriers exist or all are saturated, the datagram is rejected with a
  typed error and log/metric event;
- inbound `opaque_datagram` frames from registered carriers are reassembled when
  needed and delivered with their source carrier handle.

The Linux VPN runtime is implemented as `TunTunnelAdapter` on top of that
generic transport:

- client-side outbound TUN packets are submitted as opaque datagrams;
- server-side outbound TUN packets with an active lease pool select carriers by
  IPv4 destination: packets for a leased client address are sent only through a
  carrier owned by that client;
- multiple carriers for one client are used round-robin;
- inbound datagrams from registered carriers are accepted as TUN packets only
  after lease checks. With an active server lease pool, IPv4 source must match
  the lease assigned to that carrier.

Duplicate UUID policy:

- one UUID is intended to identify one active client device/profile, not a group
  of independent machines;
- multiple carriers from the same active client instance are valid and may be
  used for scheduling;
- when a different active instance authenticates with the same UUID, the only
  supported product policy is `replace_old`: the newer instance supersedes older
  carriers for that UUID/lease;
- round-robin routing across different devices that share one UUID is forbidden,
  because both devices would share the same leased IPv4 address and return TUN
  packets could be delivered to the wrong machine.
- `fps_client` generates a random per-process `client_instance_id` at startup
  and reuses it for all carriers created by that process;
- the instance id is sent only inside encrypted post-auth control metadata,
  never in config, lease files, logs, status output or wire plaintext;
- server-side carrier metadata stores the assigned lease IPv4 address and
  encrypted instance id. A new carrier with the same lease and same instance id
  joins the pool; a new carrier with the same lease and a different instance id
  removes/closes older carriers before it becomes active.

Opaque datagram fragmentation:

- logical `opaque_datagram` is used for datagrams `<= codec.max_frame_payload`;
- larger datagrams are split into `opaque_datagram_fragment`;
- fragment header inside encrypted frame payload contains `packet_id`,
  `fragment_index`, `fragment_count` and `total_size`;
- all fragments of one datagram stay on one carrier in the current increment;
- inbound reassembly is keyed by source carrier and `packet_id`, so fragments
  from different carriers or different datagrams can be interleaved without
  sharing state;
- active reassembly state is bounded; excess new fragmented packets are dropped
  with metadata-only counters/log events;
- malformed, out-of-order, mismatched or oversized fragments are dropped/reset
  per affected packet without logging packet bytes.

Server-assigned IPv4 leases:

- the server can own a private pool through `tun.lease_pool`,
  `tun.server_address` and persistent `tun.lease_file`;
- lease key is the authenticated client public key. The lease file stores only
  public-key/base64 to IPv4 mappings, never UUIDs or private keys;
- after Zero-RTT auth, a TUN-enabled client sends an encrypted `control` frame
  containing client-instance metadata. With a server lease pool, the server
  registers the carrier only after that metadata arrives;
- after registration, the server sends an encrypted `control` frame containing
  a `tun_lease`: version, IPv4 family, prefix length, client IPv4, server IPv4,
  network IPv4 and MTU;
- the client applies the lease only when `tun.auto_configure=true`, using
  `ip addr replace <lease>/<prefix> dev <tun>` and
  `ip link set dev <tun> up mtu <mtu>`;
- `tun.client_isolation=true` by default: the server drops inbound IPv4 TUN
  packets addressed to another leased client in the same pool;
- strict source-IP enforcement is active on the server with a lease pool:
  non-IPv4 packets, packets without lease metadata and spoofed source IPs are
  dropped before writing to the server TUN.

## 7. Shaper

Current implemented scope:

- deterministic profile-driven budget for externally injected datagram/control
  frames;
- adaptive record-size and inter-record-delay CDF training from observed
  carrier TLS records;
- size-aware classified-record encoding: when the shaper selects a target TLS
  Application Data record wire size, FPS pads the encrypted classified record to
  that exact outer TLS record size;
- shaper-aware datagram fragmentation: if a queued `opaque_datagram` is too
  large for the sampled TLS record but the record can carry at least one
  fragment header plus one byte of data, FPS expands that datagram into ordered
  `opaque_datagram_fragment` records on the same carrier; if even the smallest
  fragment cannot fit, the datagram stays queued and injection is blocked for
  that scheduling attempt;
- commit-safe shaper scheduling: rejected target sizes, padding limits and
  write-queue pressure leave queued covert data and shaper budget intact;
- backpressure accounting and bounded write queues;
- fragmentation lets near-MTU TUN packets be sent as smaller covert frames.

`shaper.record_size_cdf_c2s` and `shaper.record_size_cdf_s2c` buckets are full
outer TLS record wire sizes, including the 5-byte TLS record header. A sampled
size smaller than the classified-record overhead, or larger than the configured
classified-record padding capacity, blocks injection until another scheduling
attempt. `codec.max_frame_padding` also limits classified-record padding in the
current Linux relay config. `shaper.inter_record_delay_us_cdf_c2s` and
`shaper.inter_record_delay_us_cdf_s2c` buckets are inter-record delays in
microseconds. Public CDF config uses compact `[value, cumulative_probability]`
pairs, for example `[[512, 0.4], [1500, 1.0]]`.

Adaptive behavior:

- each relay process owns one shared `Shaper` instance when the shaper is
  enabled; all authenticated and pre-auth carrier sessions in that process
  train the same adaptive model;
- observations are made after TCP bytes have been sliced into complete TLS
  records, so `recv`/`read_some` chunk boundaries do not affect the model;
- only ordinary forwarded carrier TLS records train the adaptive CDF. Inserted
  classified FPS records are excluded from observations;
- adaptive scheduling is used only after the configured minimum record count and
  minimum observed time window are reached for a direction. Until then, the
  static configured CDF remains the bootstrap/fallback model;
- default warmup is 16 observed carrier records and 2000 ms per direction;
- adaptive buckets are decayed with `adaptive.decay` to keep the model inertial
  while still allowing it to follow long-running carrier changes;
- the server periodically sends an encrypted shaper snapshot control frame to
  authenticated clients. The snapshot contains CDF metadata and profile id only,
  no payload bytes, keys, UUIDs or nonces. Clients treat it as advisory model
  bootstrap data for faster convergence after authentication.

Deferred work:

- statistical assertions for record size/delay distributions;
- classifier regression lab and repeated pcap-level comparison against
  representative carrier profiles.

## 8. Configuration

Config format: JSON parsed through Boost.JSON.

Minimal server-side v5 shape:

```json
{
  "network": {
    "listen": "127.0.0.1:8443",
    "origin": "127.0.0.1:9443",
    "read_buffer_size": 65536,
    "tcp_no_delay": true
  },
  "security": {
    "zero_rtt": {
      "enabled": true,
      "profile_id": "example-origin-v5",
      "server_private_key_base64": "PASTE_server_private_key_base64_HERE",
      "server_public_key_base64": "PASTE_server_public_key_base64_HERE",
      "allowed_client_uuids": ["123e4567-e89b-42d3-a456-426614174000"],
      "version": 5,
      "min_records_before_trial": 1,
      "upgrade_direction": "client_to_server",
      "client_upgrade_delay_ms": 0,
      "client_upgrade_delay_sigma_ms": 0
    }
  },
  "codec": {
    "max_frame_payload": 1280,
    "max_frame_padding": 64,
    "allow_fragmentation": true
  },
  "tun": {
    "enabled": true,
    "name": "fps0",
    "mtu": 1280,
    "max_write_queue_packets": 64,
    "lease_pool": "10.66.0.0/30",
    "server_address": "10.66.0.1",
    "lease_file": "leases.json",
    "client_isolation": true
  },
  "limits": {
    "max_session_write_queue_bytes": 1048576
  },
  "logging": {
    "level": "info"
  },
  "ops": {
    "status_socket": "/run/fps/server.status"
  }
}
```

Client config uses `network.server`, `client_uuid`,
`server_public_key_base64` and, when desired, `tun.auto_configure=true`. Server
config uses only inline padded RFC4648 base64 fields
`server_private_key_base64`/`server_public_key_base64` and
`allowed_client_uuids`.
`security.zero_rtt.version` is optional and defaults to `5`. When present it
must be `5`; pre-production wire formats are intentionally not accepted as
compatibility modes.
`security.zero_rtt.client_upgrade_delay_ms` defaults to `2000` for client
configs and `0` for server configs. The delay is checked when more carrier TLS
records arrive after bidirectional Application Data is observed; it is intended
for continuous carrier sessions, not as a wall-clock timer that injects FPS
bytes into an idle carrier.
`security.zero_rtt.client_upgrade_delay_sigma_ms` defaults to one third of
`client_upgrade_delay_ms` for client configs and `0` for server configs. Each
client carrier samples one effective delay when the bidirectional TLS
Application Data channel first becomes eligible for upgrade:
`clamp(client_upgrade_delay_ms + N(0, sigma_ms), 0, 2 * client_upgrade_delay_ms)`.
Set the sigma field to `0` for reproducible tests and packet-capture
experiments. FPS still never sends the client auth record before observing TLS
Application Data in both carrier directions and the required transcript records.

Optional shaper adaptive fields live under `shaper.adaptive`:

```json
{
  "shaper": {
    "enabled": true,
    "profile_id": "example-origin-v5",
    "record_size_cdf_c2s": [[512, 0.4], [1500, 1.0]],
    "record_size_cdf_s2c": [[512, 0.4], [1500, 1.0]],
    "inter_record_delay_us_cdf_c2s": [[20000, 0.5], [100000, 1.0]],
    "inter_record_delay_us_cdf_s2c": [[20000, 0.5], [100000, 1.0]],
    "adaptive": {
      "enabled": true,
      "min_records": 16,
      "min_observation_ms": 2000,
      "decay": 0.98,
      "snapshot_interval_ms": 30000
    }
  }
}
```

`network.tcp_no_delay` defaults to `true` and is applied to both accepted and
outbound FPS relay TCP sockets. This disables Nagle on the FPS link so shaped
classified TLS records are not delayed or coalesced by an implicit TCP policy.
Other TCP knobs such as corking, socket buffer sizes, keepalive and quick ACK
are intentionally not part of the beta config; batching belongs to the shaper,
and platform-specific TCP heuristics can create new fingerprints.

Validation rules:

- `tun.enabled=true` requires `security.zero_rtt.enabled=true`;
- `codec.max_frame_payload` must be positive;
- with `codec.allow_fragmentation=true` and TUN,
  `codec.max_frame_payload` must be larger than the fragment header;
- with `codec.allow_fragmentation=false`, `tun.mtu` must not exceed
  `codec.max_frame_payload`;
- server `tun.lease_pool` requires `tun.lease_file`, and `tun.server_address`
  must be a usable IPv4 address inside the pool.

Lease-management CLI:

- `fps_server --lease-list --config server.json` prints JSON summary with pool,
  server address, IP leases, public-key fingerprints and allowlist status;
- `fps_server --lease-revoke-client-uuid UUID --config server.json` removes the
  lease for the UUID-derived client public key; idempotent `not_found` exits 0;
- `fps_server --lease-prune --config server.json` removes leases that are no
  longer derived from current `allowed_client_uuids`;
- UUID values and full public/private keys are not printed by default.

Operational status:

- optional `ops.status_socket` enables a local UNIX socket;
- each connection receives one JSON snapshot and is then closed;
- `fps_client --status --config client.json` and
  `fps_server --status --config server.json` query the configured socket;
- `--status-socket PATH` overrides the config path for ad-hoc queries;
- status JSON contains role, pid, uptime, listen/target endpoints, session and
  carrier lifecycle counters, duplicate UUID replacement counters,
  `sessions.last_closed`, bounded `sessions.recent_closed`, auth counters under
  `auth`, classified-record counters under `classified_record`, TUN packet/drop
  counters, shaper/backpressure counters and, when enabled,
  `shaper.profile` with a non-secret compact CDF snapshot;
- `auth` contains candidate, authenticated, precheck failure, unknown-client,
  decrypt failure and server-accept failure counters;
- `classified_record` contains decode/encode failure, tamper/invalid and
  records-decoded/records-encoded counters;
- close metadata contains only `session_id`, authentication state, close reason
  and non-secret direction/component/stage/error names;
- status output must not include UUID values, private/public keys, raw client
  instance ids, raw TLS payloads, raw TUN packets or IP payload bytes.

Shaper profile export CLI:

- `fps_client --write-shaper-profile --config client.json --output profile.json
  [--force]` and the same `fps_server` command write a normalized shaper profile
  JSON file with `0600` permissions;
- if `ops.status_socket` or `--status-socket PATH` is reachable, the exported
  profile uses the live adaptive CDF snapshot from `shaper.profile`;
- otherwise the command falls back to the static profile from the config;
- only `--format json` is supported in the current schema.

Server keypair CLI:

- `fps_server --generate-server-keypair` prints text fields for manual use;
- `fps_server --generate-server-keypair --format json` prints a flat JSON object
  with `server_private_key_base64` and `server_public_key_base64`, matching the
  server config field names;
- unsupported keypair formats fail config/CLI parsing with a diagnostic.

Offline shaper profile tooling:

- `tools/pcap_to_shaper_profile.py carrier.pcap --port 443 --output
  profile.json` builds the same compact JSON shaper profile from a captured TLS
  carrier TCP session;
- the tool uses libpcap, reassembles each TCP direction by sequence number and
  parses TLS records independently of packet or `recv` boundaries;
- client/server direction is inferred from the TCP SYN/SYN-ACK handshake when
  present. If the pcap starts after the handshake, `--port PORT` is used as a
  service-port hint;
- by default only TLS Application Data records are sampled. The record-size CDF
  uses full TLS record wire size including the 5-byte TLS header, matching the
  runtime shaper observation path; delay CDFs are inter-record delays in
  microseconds;
- the pcap should represent carrier-only baseline traffic or a deliberately
  selected pre-upgrade window. A pcap that already contains shaped FPS records
  describes the combined visible flow, not the original carrier baseline.

Client profile CLI:

- `fps_server --generate-client-profile --config server.json --client-uuid UUID
  --server-endpoint HOST:PORT` prints a valid `fps_client` JSON profile;
- `--client-status-socket PATH` adds `ops.status_socket` to that generated
  client profile when the operator wants status to work immediately;
- `--format uri` prints the same profile as `fps://v1/<base64url-json-profile>`;
- `--output PATH [--force]` writes generated JSON or URI output as secret
  material with `0600` permissions; existing files are not overwritten unless
  `--force` is set;
- `fps_client --print-config-from-uri URI` decodes an `fps://v1` URI back to
  client JSON;
- `fps_client --write-config-from-uri URI --output PATH [--force]` decodes and
  writes client JSON with the same secret-file overwrite rules;
- `fps://v1` decoding is implemented in platform-neutral core. It normalizes the
  JSON profile and validates the client UUID plus server public key before the
  Linux CLI writes or prints it;
- the generated profile contains `client_uuid`, `server_public_key_base64`,
  profile id, carrier endpoint, codec settings and client-side TUN
  auto-configuration defaults;
- `--generate-client-uuid` prints only one raw canonical UUID line;
- UUID inputs must be raw canonical UUID strings;
- the generated profile must not contain `server_private_key_base64`,
  `allowed_client_uuids`, lease file paths or server lease-pool internals;
- profile generation rejects UUIDs that are not currently allowlisted.

## 8.1 Platform Boundary

`fps_core` is the narrow platform-neutral layer intended for Android reuse:
crypto, Zero-RTT, classified-record codec, `fps://v1` client profile
normalization and generic datagram scheduling. TUN framing/adaptation and the
TLS/TCP carrier are explicit opt-in targets above that core. The current Android
scaffold adds a headless Kotlin runtime boundary: it parses client JSON and
`fps://v1` profiles, models carrier probes and split-tunnel allowlists, models
the VPN startup state machine, provides live OkHttp HTTPS/WSS probe/keepalive
traffic for app-owned carrier sessions, owns the first lease-triggered Android
`VpnService` TUN file descriptor, keeps platform operations behind hooks, and
builds an NDK library that reuses FPS native core pieces without linking Linux
runtime code.

Linux-specific runtime is separate:

- `fps_linux_runtime` contains relay CLI app, Linux TUN open and production
  `TunRuntime`;
- `TunRuntime` is injected into the relay app and provides TUN opening plus
  semantic link/address operations. The Linux implementation translates those
  operations to no-shell `ip` execution; Android should later back the same
  operations with `VpnService`;
- unit tests use fake runtime/configurator objects and MockWebServer for live
  OkHttp carrier-probe checks. Android now has a first `VpnService.Builder`
  fd ownership adapter and JNI runtime handle; native auth/pump wiring and
  richer Android network configuration remain follow-up work;
- Android callbacks must not call carrier enqueue from arbitrary JNI/Kotlin
  threads. They must post work onto the FPS/carrier executor or use a future
  async adapter API.
- Outbound TCP carrier connects use an injectable protector boundary. The
  current Linux runtime passes a no-op protector; Android native sockets use
  the fd hook, while OkHttp-owned carrier sockets use the Java `Socket` hook
  before connect;
- The first Android direction is app-owned carrier sessions, socket protection
  through `TcpSocketProtector`/platform hooks, hostname resolution through
  Android's underlying network, two-phase lease-before-TUN startup and split
  tunnel by default.
- The current Kotlin headless layer can derive carrier probe plans from
  profile metadata, resolve those endpoints through the underlying-network hook,
  drive a deterministic fake-transport carrier probe manager and open live
  OkHttp HTTPS/WSS probe sockets with protect-before-connect ordering and
  reconnect/backoff status. These OkHttp sockets are not FPS wire carriers
  because they terminate TLS in the Android HTTP stack; the real FPS carrier
  path must use native raw TCP/TLS stream handling through
  `TlsTcpCarrierSession`. Android can establish and own a `VpnService` fd after
  a server lease, installing the leased IPv4 address and leased-subnet route. It
  requires `tun.enabled=true` before lease-triggered TUN establishment, exposes
  non-secret runtime snapshots, and has a headless Kotlin/native lifecycle
  bridge that attaches the borrowed fd to a native runtime handle with explicit
  `tunFdOwnership=borrowed` metadata. It does not yet start the native auth path
  or TUN pump. Future native pump wiring must duplicate the fd or use a separate
  native-owned attach path before native code may close it.
- TUN adapters can install an outbound packet policy hook before covert
  enqueue. The hook receives raw packet bytes plus a best-effort parsed IPv4
  TCP/UDP 5-tuple (`protocol`, source/destination IPv4 and ports). Android
  should use this boundary to call the platform connection-owner API and
  fail closed for UIDs outside the configured split-tunnel allowlist. The hook
  must not log UUIDs, keys, raw packets or payload bytes.
- The current Android scaffold links a native smoke boundary made of protocol
  codec/crypto, generic covert datagram transport, TLS/TCP carrier sources and
  the TUN 5-tuple parser. Kotlin owns the first Android profile parser instead
  of pulling Linux Boost.JSON config code into the app. Android OpenSSL is
  cross-built through vcpkg only for Android triplets; Boost.Asio/Boost.System
  are used header-only in that smoke. Linux relay/config/CLI, Linux TUN device
  code, Boost.Log and Boost.JSON-heavy operator paths remain outside Android
  targets. Header-only Boost.Describe/MP11/Endian remain usable on Android
  through an isolated Boost header root.
- Android build/unit checks are reproducible through `Dockerfile.android`.
  Runtime validation is intentionally separate: the connected instrumented smoke
  loads the native library and calls the core smoke on an attached device or
  emulator, but emulator execution is not required for ordinary Linux CI.

## 9. Observability

Runtime logs use the project `FPS_LOG_*` facade. Linux uses Boost.Log behind
that facade; Android native code uses an `__android_log_print` backend at the
same macro boundary.
Service structs that carry operational counters or state should be annotated
with Boost.Describe and logged through the project describe-to-JSON helper
instead of repeating every field by hand. The current log sink still emits text
fields such as `event=session_stats stats={...}`, but the structured tail is
valid JSON so operators can gradually move from `grep` to `jq`-style tooling.

Allowed logs:

- lifecycle: start/listening/target connected/session closed;
- Zero-RTT auth/build/candidate-miss metadata;
- carrier registered/removed and carrier count;
- TUN open/closed/errors, queue/backpressure/drop counters;
- shaper queued/blocked/scheduled events;
- parser/record/classified-record errors without payload bytes.

Forbidden logs:

- private keys, shared secrets, raw upgrade plaintext;
- nonces, session keys;
- raw TLS payloads, raw classified records, raw TUN packets;
- full IP payload bytes.

## 10. Testing Baseline

Ordinary non-sudo `ctest` should cover:

- unit tests: TLS parser/layer, Zero-RTT, classified-record/frame-bundle codec,
  shaper, generic datagram transport, TUN adapter enforcement/framing,
  config/CLI/logging;
- local integration: HTTPS passthrough, HTTPS Zero-RTT chain, concurrent
  Zero-RTT sessions, reusable HTTPS/WSS carrier passthrough and WSS Zero-RTT.

Opt-in sudo/TUN suite should cover:

- TUN open smoke;
- Zero-RTT TUN loopback in isolated network namespaces;
- burst, fragmentation, shaper and multi-carrier variants.

Quality/safety checks also include clang-20 warning build, ASan/UBSan, Valgrind
unit pass, llvm-cov gate and bounded libFuzzer smoke for TLS record parsing,
covert frame-bundle/classified-record decode, Zero-RTT candidates and
TUN/control payload parsing.
Product-level Docker simulations cover one-client UDP `iperf3`, two-client lease
routing/spoof-drop, duplicate UUID `replace_old` behavior and the official Dante
SOCKS5 overlay example smoke.

See [testing.md](./testing.md).

## 11. Roadmap

Near productionization gaps:

- QR/mobile onboarding on top of the existing `fps://v1` profile URI;
- deeper Zero-RTT DoS review beyond transcript-bound precheck;
- research-grade handshake-less classified-record classification where FPS records and
  ordinary carrier records can coexist without an explicit upgrade state;
- adversarial active-probe/tamper/drop test plan;
- automated pcap/tshark regression checks in CI-capable environments;
- external protocol/security review of the current transcript-bound
  classified-record construction;
- automated or scheduled privileged two-host Docker/TUN soak;
- public release signing, upgrade guide and root/TUN CI policy;
- routing/NAT deployment guides beyond isolated namespaces and current proxy
  overlay examples;
- lease rotation/revocation UX beyond basic list/revoke/prune;
- long-running backpressure and traffic-shape regression lab.

See [beta-status.md](./beta-status.md) and
[client-profiles.md](./client-profiles.md).

## 12. Terms

- **FPS link**: TCP connection between `fps_client` and `fps_server`.
- **Cover TLS session**: real TLS session whose ordinary TLS records FPS proxies
  byte-for-byte before and after upgrade.
- **Zero-RTT upgrade**: the current config namespace for the late carrier
  authentication path. The active wire flow uses an encrypted client auth record
  followed by an encrypted server accept record before final classified-record
  keys are used.
- **Classified FPS record**: encrypted FPS datagram/control record inserted as a
  TLS Application Data record and consumed by the FPS peer before reaching the
  real TLS endpoint.
- **Carrier**: authenticated TLS session available for opaque datagram/control
  frames.
- **Carrier pool**: set of authenticated carriers without primary ownership.
- **Opaque datagram**: best-effort payload carried by FPS. The current Linux
  runtime maps one TUN packet to one opaque datagram before any internal
  fragmentation.
- **TUN packet**: IP packet from a Linux TUN device.
