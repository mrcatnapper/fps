# Android Boundary Hardening

This note records the current tactical plan before Android application work
starts. It is a developer handoff document, not user/operator documentation.

## Boundary State

- `CovertDatagramTransport` is the right reusable transport seam: it schedules
  opaque datagrams across `CovertCarrier` handles and does not know whether the
  payload is an IPv4 packet, a future app datagram, SSH data or another adapter.
- `TunTunnelAdapter` registers generic `CovertCarrier` handles and tracks only
  `CarrierId`, lease IPv4 and encrypted client-instance metadata. It no longer
  includes or stores `TlsTcpCarrierSession`.
- `fps_core` is the narrow Android-facing aggregate: protocol core plus generic
  datagram core plus reusable `fps://v1` client profile normalization. The
  current TLS/TCP carrier and TUN adapter are explicit opt-in targets above it.
- `TunRuntime` is injectable and exposes semantic operations: open TUN, set link
  MTU, bring link up and replace IPv4 address. Linux `ip` argv generation is
  contained in the Linux runtime.
- `CovertCarrier` enqueue is a synchronous same-executor contract. The transport
  checks `can_enqueue_now` when provided and rejects wrong-thread calls before
  touching session queues.

## Implemented In This Increment

- Decoupled `TunTunnelAdapter` from `TlsTcpCarrierSession`.
- Registered carriers with generic `CovertCarrier` plus FPS metadata:
  `CarrierId`, optional assigned client IPv4 and optional encrypted client
  instance id.
- Returned replaced carrier ids from duplicate-UUID handling. The Linux relay
  runtime owns the `CarrierId -> TlsTcpCarrierSession` map and stops replaced
  sessions.
- Passed inbound decoded frames to the TUN adapter by `CarrierId`, not by session
  pointer.
- Split CMake targets so `fps_tun_adapter` links the generic datagram core, not
  the concrete TLS/TCP carrier target.
- Replaced the Linux-shaped `run_ip_command(...)` boundary with semantic
  `TunRuntime` link/address operations.
- Made carrier enqueue executor affinity explicit through `CovertCarrier` and
  the TLS/TCP carrier adapter.
- Moved `fps://v1` client profile URI encode/decode and profile JSON
  normalization into platform-neutral core.

## Follow-Up Increments

- Decide how much full relay config parsing Android should reuse directly,
  beyond the shared `fps://v1` profile import layer.
- Define the Android `VpnService` design: TUN fd ownership, `protect()` for
  carrier sockets, DNS/route behavior, lifecycle/reconnect and status reporting.
- Decide whether Android should keep the same synchronous executor-only
  contract or introduce a separate async adapter for UI/JNI-facing calls.
