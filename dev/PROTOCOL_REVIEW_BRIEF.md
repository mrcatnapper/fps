# FPS Protocol Review Brief

Date: 2026-05-17

This brief is for an independent cryptographic/protocol reviewer. It summarizes
the current beta design and points to the implementation and test evidence that
should be reviewed before public beta.

## Review Scope

- Zero-RTT carrier authentication and indexed precheck.
- Encrypted FPS envelope mode over TLS Application Data shaped records.
- Replay cache and channel-binding policy.
- Carrier pool and leased-client TUN routing semantics.
- Metadata-only logging/status rules.

Out of scope for this review pass: advanced timing/size shaping, Android
`VpnService`, IPv6 allocation, proxy overlay policy and native packaging.

Primary references:

- Public spec: `docs/specification.md`
- Testing workflow: `docs/testing.md`
- Beta risk register: `docs/beta-status.md`
- Core tests: `tests/test_zero_rtt.cpp`, `tests/test_fps_envelope.cpp`,
  `tests/test_session_manager.cpp`
- Fuzz targets: `tests/fuzz/`

## Threat Model Summary

FPS is a Linux-first hidden L3 TUN tunnel carried inside live TLS cover
sessions. On the `fps_client <-> fps_server` link, an observer should see a TCP
stream of TLS Application Data records. FPS does not terminate the real
browser/origin TLS session; real TLS bytes are packed into encrypted FPS
envelopes after upgrade and restored before they reach the real endpoints.

The current beta target is controlled operator deployment, not resistance to a
state-level traffic-analysis adversary. Advanced timing/size shaping remains
deferred.

## Zero-RTT Authentication

Current construction:

- Client identity is a secret `client_uuid` that deterministically derives an
  internal X25519 client key pair.
- Server identity is an inline base64 X25519 key pair plus an allowlist of
  client UUIDs.
- The client sends one encrypted upgrade candidate after observing a previous
  real TLS record.
- Upgrade associated data binds the attempt to direction, role, TLS record
  index, previous-record hash and profile id.
- Server-side indexed precheck decrypts a small capsule, maps internal ClientID
  to one allowlisted client public key and then attempts one full decrypt.
- The server confirmation envelope is race-safe: the client trial-decrypts
  plausible peer records and forwards non-confirmation records as cover traffic
  until confirmation succeeds or policy expires.
- Replay protection targets captured-prefix replay. Replaying only an auth
  record into another organic TLS context should fail channel binding because
  `hash(previous TLS record)` and record index differ. Replaying the previously
  captured previous-record plus auth-record prefix can reconstruct the same
  channel binding, so the daemon-wide in-memory replay cache must reject it.

Known risk: the current candidate starts with a fixed-position 32-byte
ephemeral public key. Even if intended to be fresh random material, this is a
high-priority visible-prefix review item because it is easier to classify than
timing. The next wire revision should hide or replace it with a short
time/window-bound opaque lookup hint.

Candidate next direction: bind candidates to a per-direction transcript hash of
all carrier bytes observed before the candidate record, seeded and
domain-separated by server public key, profile id, role, direction and protocol
version. This would make ordinary cross-session replay depend on reproducing the
same carrier byte prefix, which should be infeasible with an honest/non-malicious
origin once real TLS Application Data has started. Timestamp and replay-cache
checks should remain defense-in-depth for artificially replayed prefixes,
daemon restarts and implementation bugs.

The same transcript-bound material may also support a future CPU-friendly
classifier:

- reject records whose server-side transcript hint does not match;
- scan the allowlist only for records whose client-side transcript hint matches;
- run full cryptographic verification only for the likely client.

This is not a full active DoS defense because public hints can be forged by an
attacker that knows the construction. Its main value is removing the visible
public-key-shaped prefix and avoiding full decrypt work for ordinary random
carrier records.

Reviewer questions:

- Is the channel binding sufficient for the late-upgrade transparent relay
  topology?
- Is the precheck capsule acceptable as a CPU-friendly lookup without becoming a
  durable client identifier?
- Are timestamp/replay cache semantics sufficient when the current cache is
  daemon-process memory and intentionally clears on restart?
- Is captured-prefix replay the right beta threat to cover, and is restart-clear
  replay state acceptable for controlled deployments?
- Should the visible-prefix refactor block public beta or be tracked as a
  post-beta wire revision?
- Should the next wire revision use a full carrier transcript hash rather than
  only the previous TLS record hash?
- Can transcript-bound server/client hints be designed without creating a
  durable client identifier or fragile false-positive classifier?

## Envelope Mode

After authentication, visible FPS records are TLS Application Data records whose
payload is an AEAD-encrypted FPS envelope. Envelope plaintext includes:

- inner real TLS bytes;
- covert TUN/control frames;
- padding size and frame metadata.

Sequence numbers are implicit per direction and are used for AEAD nonce
discipline. No plaintext FPS frame headers are sent directly over TCP. After
upgrade, decrypt/tamper failures close or drain the carrier rather than
silently preserving cover.

Reviewer questions:

- Is the implicit sequence/nonce discipline sound for long-lived TCP carriers?
- Are envelope failure semantics correct for a transparent browser/origin TLS
  stream?
- Are metadata boundaries sufficient, or should more fields be padded/hidden in
  the next wire revision?
- Is a future handshake-less mode viable, where each TLS Application Data record
  is classified as ordinary carrier bytes or an FPS envelope using
  transcript-bound one-time hints plus AEAD verification, without first switching
  the whole carrier into envelope mode?

## Lease Routing And Client Isolation

Server-owned IPv4 leases are assigned after Zero-RTT auth. The server records
the assigned client IPv4 as carrier metadata.

Current enforcement:

- Client-to-server TUN packets are accepted only if IPv4 source equals the
  carrier's assigned lease.
- Server-to-client TUN packets are routed only to carriers that own the
  destination lease.
- Duplicate UUID policy is `replace_old`: a newer active process instance for
  the same UUID supersedes older carriers for that UUID.
- Client-to-client traffic is dropped when `tun.client_isolation=true`.

Reviewer questions:

- Does lease enforcement cover the relevant malicious-client cases for a beta
  L3 VPN?
- Are duplicate UUID semantics safe enough for accidental shared UUIDs?
- Are there routing cases that should become hard errors rather than drops?

## Logging And Status Secrecy

Logs/status must not include UUIDs, private keys, derived keys, ClientID values,
nonces, session keys, raw upgrade material, raw TLS payloads, raw TUN packets or
IP payload bytes.

Status is a local UNIX-socket JSON snapshot, not a management API. It exposes
session counters, recent close metadata, auth and envelope counters, TUN
counters and lease metadata counts.

Reviewer questions:

- Are any current metadata fields linkable enough to be treated as sensitive?
- Should lease-list fingerprints be truncated further before public beta?

## Evidence Already Available

- Unit coverage for Zero-RTT success/failure, replay, timestamp expiry, indexed
  precheck, shared daemon replay cache behavior, envelope roundtrip/tamper and
  lease routing.
- Local integration coverage for HTTPS/WSS passthrough, Zero-RTT chains,
  large/coalesced responses, multi-session carriers, unknown clients,
  captured-prefix replay and post-auth envelope tamper.
- TUN/netns coverage for loopback, burst, fragmentation and shaper-adjacent
  paths.
- Docker simulations for multi-client lease routing, duplicate UUID replacement,
  SOCKS overlay and resilience smoke.
- One 30-minute two-host Docker/TUN soak over a real published `:443` carrier
  path on 2026-05-17.
- Bounded libFuzzer smoke for TLS record parsing, covert frames, FPS envelope
  decode, Zero-RTT candidates and TUN/control parsing.

## Known Non-Blocking Beta Risks

- No formal proof of the Zero-RTT/envelope construction.
- Visible Zero-RTT public-key-shaped prefix needs a future wire revision.
- Indexed precheck is not a full CPU DoS defense; plausible candidates still
  cost one X25519 and one small AEAD attempt.
- Replay cache persistence across server restarts is not yet configurable; the
  beta baseline is shared in-memory daemon state.
- Advanced timing/size traffic shaping is deferred.
