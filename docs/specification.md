# FPS Protocol And Architecture Specification

Version: 0.10, 1-RTT Zero-RTT finalization beta increment

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

Linux is the active target platform for both client and server. Android remains
future work through `VpnService`; the current build separates protocol core,
carrier/datagram core, TUN adapter and Linux runtime targets so future adapters
can reuse authenticated carriers without inheriting Linux TUN or `ip`
orchestration.

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
  Application Data in both carrier directions and enough real carrier TLS
  records to form a bidirectional transcript binding.
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

- `add_carrier_session` is called only after Zero-RTT authentication;
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
- backpressure accounting and bounded write queues;
- fragmentation lets near-MTU TUN packets be sent as smaller covert frames.

Deferred work:

- full timing/size control of inserted classified FPS records against the
  observed carrier profile;
- statistical assertions for record size/delay distributions;
- profile capture tooling and classifier regression lab.

## 8. Configuration

Config format: JSON parsed through Boost.JSON.

Minimal server-side v5 shape:

```json
{
  "network": {
    "listen": "127.0.0.1:8443",
    "origin": "127.0.0.1:9443",
    "read_buffer_size": 65536
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
      "upgrade_direction": "client_to_server"
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
  counters and shaper/backpressure counters;
- `auth` contains candidate, authenticated, precheck failure, unknown-client,
  decrypt failure and server-accept failure counters;
- `classified_record` contains decode/encode failure, tamper/invalid and
  records-decoded/records-encoded counters;
- close metadata contains only `session_id`, authentication state, close reason
  and non-secret direction/component/stage/error names;
- status output must not include UUID values, private/public keys, raw client
  instance ids, raw TLS payloads, raw TUN packets or IP payload bytes.

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
- the generated profile contains `client_uuid`, `server_public_key_base64`,
  profile id, carrier endpoint, codec settings and client-side TUN
  auto-configuration defaults;
- `--generate-client-uuid` prints only one raw canonical UUID line;
- UUID inputs must be raw canonical UUID strings;
- the generated profile must not contain `server_private_key_base64`,
  `allowed_client_uuids`, lease file paths or server lease-pool internals;
- profile generation rejects UUIDs that are not currently allowlisted.

## 8.1 Platform Boundary

`fps_core` is the platform-neutral layer intended for future Android reuse:
crypto, Zero-RTT, classified-record codec, session/carrier scheduling, TUN framing,
lease/control payloads and TUN packet pump do not depend on Linux `ip` or
`/dev/net/tun`.

Linux-specific runtime is separate:

- `fps_linux_runtime` contains relay CLI app, Linux TUN open and production
  `TunRuntime`;
- `TunRuntime` is injected into the relay app and provides TUN opening plus
  no-shell `ip` execution;
- unit tests use fake runtime/configurator objects. Android should later provide
  a `VpnService` file descriptor and Android network configurator.

## 9. Observability

Runtime logs use the project `FPS_LOG_*` facade over Boost.Log.
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
- production key/UUID rotation docs;
- public Docker operator examples for self-hosted carrier, proxy overlays and
  routing;
- routing/NAT deployment guides beyond isolated namespaces;
- lease rotation/revocation UX beyond basic list/revoke/prune;
- long-running soak/backpressure tests.

See [beta-status.md](./beta-status.md) and
[client-profiles.md](./client-profiles.md).

## 12. Terms

- **FPS link**: TCP connection between `fps_client` and `fps_server`.
- **Cover TLS session**: real TLS session whose ordinary TLS records FPS proxies
  byte-for-byte before and after upgrade.
- **Zero-RTT upgrade**: one encrypted auth record that authenticates a carrier
  and enables classified FPS record insertion.
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
