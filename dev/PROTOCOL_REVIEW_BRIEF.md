# FPS Protocol Review Brief

Date: 2026-05-17

This brief is for an independent cryptographic/protocol reviewer. It summarizes
the current beta design and points to the implementation and test evidence that
should be reviewed before public beta.

## Review Scope

- Zero-RTT carrier authentication and transcript-bound hint precheck.
- Classified FPS records inserted as TLS Application Data shaped records.
- Transcript binding and no-cache replay policy.
- Carrier pool and leased-client TUN routing semantics.
- Metadata-only logging/status rules.

Out of scope for this review pass: advanced timing/size shaping, Android
`VpnService`, IPv6 allocation and proxy overlay policy.

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
browser/origin TLS session; ordinary carrier TLS records remain byte-for-byte
visible after upgrade while FPS inserts separate classified records for
TUN/control data.

The current beta target is controlled operator deployment, not resistance to a
state-level traffic-analysis adversary. Advanced timing/size shaping remains
deferred.

## Zero-RTT Authentication

Current construction:

- Client identity is a secret `client_uuid` that deterministically derives an
  internal X25519 client key pair.
- Server identity is an inline base64 X25519 key pair plus an allowlist of
  client UUIDs.
- The client sends one encrypted upgrade candidate after observing enough real
  carrier TLS records to form a transcript binding.
- Upgrade associated data binds the attempt to direction, TLS record index,
  transcript byte count, transcript hash and profile id.
- The auth record starts with `server_hint[8] | client_hint[8]`; the client
  ephemeral public key is inside the encrypted capsule, not visible at a fixed
  wire offset.
- Server-side precheck rejects wrong server hints, scans the UUID allowlist for
  a matching client hint and then attempts one capsule decrypt for the likely
  client.
- The server accept record is race-safe: the client classifies plausible peer
  records and forwards non-accept records as cover traffic until accept
  succeeds or policy expires.
- Timestamp and replay-cache fields are removed from active v5. Replay
  resistance relies on reproducing the exact bidirectional carrier transcript
  prefix and completing the server accept leg before final classified keys
  exist; durable replay state is left for reviewer feedback if this model is
  insufficient.

Reviewer questions:

- Is the channel binding sufficient for the late-upgrade transparent relay
  topology?
- Are transcript-bound hints acceptable as a CPU-friendly lookup without
  becoming durable client identifiers?
- Is the no-timestamp/no-cache replay model acceptable for controlled beta
  deployments, or should bounded/durable replay state return?
- Can transcript-bound server/client hints be designed without creating a
  durable client identifier or fragile false-positive classifier?

## Classified FPS Records

After authentication, ordinary carrier TLS records continue to be forwarded
byte-for-byte. FPS inserts separate TLS Application Data records whose payload
is an AEAD-encrypted classified FPS record. Encrypted plaintext includes:

- covert TUN/control frames;
- padding size and frame metadata.

Sequence numbers are implicit per direction and are used for AEAD nonce
discipline. Hints and AEAD associated data bind each FPS record to the current
carrier transcript snapshot. No plaintext FPS frame headers are sent directly
over TCP. Hint misses are ordinary carrier records; hint matches with failed
decrypt/validation close or drain the carrier rather than silently preserving
cover.

Reviewer questions:

- Is the implicit sequence/nonce discipline sound for long-lived TCP carriers?
- Are classified-record failure semantics correct for a transparent
  browser/origin TLS stream?
- Are metadata boundaries sufficient, or should more fields be padded/hidden in
  the next wire revision?
- Is a future no-bootstrap mode viable, where each TLS Application Data record
  is classified as ordinary carrier bytes or an FPS record using
  transcript-bound one-time hints plus AEAD verification?

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

Logs/status must not include UUIDs, private keys, derived keys, nonces, session
keys, raw upgrade material, raw TLS payloads, raw TUN packets or IP payload
bytes.

Status is a local UNIX-socket JSON snapshot, not a management API. It exposes
session counters, recent close metadata, auth and classified-record counters,
TUN counters and lease metadata counts.

Reviewer questions:

- Are any current metadata fields linkable enough to be treated as sensitive?
- Should lease-list fingerprints be truncated further before public beta?

## Evidence Already Available

- Unit coverage for Zero-RTT success/failure, transcript mismatch, hint
  precheck, classified-record roundtrip/tamper and lease routing.
- Local integration coverage for HTTPS/WSS passthrough, Zero-RTT chains,
  large/coalesced responses, multi-session carriers, unknown clients,
  transcript-prefix mismatch and post-auth carrier tamper.
- TUN/netns coverage for loopback, burst, fragmentation and shaper-adjacent
  paths.
- Docker simulations for multi-client lease routing, duplicate UUID replacement,
  SOCKS overlay and resilience smoke.
- One 30-minute two-host Docker/TUN soak over a real published `:443` carrier
  path on 2026-05-17.
- Bounded libFuzzer smoke for TLS record parsing, covert frames, FPS envelope
  decode, Zero-RTT candidates and TUN/control parsing.

## Known Non-Blocking Beta Risks

- No formal proof of the Zero-RTT/classified-record construction.
- Transcript-bound hints remove the visible public-key-shaped prefix, but the
  construction has not had independent review.
- Precheck is not a full CPU DoS defense; plausible server hints still force
  allowlist hint checks and one small AEAD attempt for a likely client.
- No timestamp or replay cache is active in v5; replay assumptions depend on
  carrier transcript uniqueness under honest origins.
- Advanced timing/size traffic shaping is deferred.
