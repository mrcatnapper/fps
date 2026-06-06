# Android Boundary Hardening

This note records the current tactical plan before Android application work
starts. It is a developer handoff document, not user/operator documentation.

## Current Findings

- `CovertDatagramTransport` is the right reusable transport seam: it schedules
  opaque datagrams across `CovertCarrier` handles and does not know whether the
  payload is an IPv4 packet, a future app datagram, SSH data or another adapter.
- `TunTunnelAdapter` still leaks the concrete TLS/TCP carrier into the TUN
  layer through `TlsTcpCarrierSession` pointers. That weakens the intended
  layering and makes future Android/VpnService work pull in more networking code
  than necessary.
- `fps_core` is currently a convenience aggregate, not the narrow Android core.
  Android should depend on protocol/datagram code first, then explicitly add
  only the carrier and adapter pieces it needs.
- `TunRuntime` is injectable, but its current command surface is Linux-shaped:
  it exposes `run_ip_command(...)`. Android will need semantic link/address
  operations backed by `VpnService`, not `ip` command arguments.
- Runtime enqueue calls are effectively same-executor operations. Android JNI or
  Kotlin callbacks must not call into session queues from arbitrary threads
  until the enqueue contract is made explicit.

## This Increment

- Decouple `TunTunnelAdapter` from `TlsTcpCarrierSession`.
- Register carriers with generic `CovertCarrier` plus FPS metadata:
  `CarrierId`, optional assigned client IPv4 and optional encrypted client
  instance id.
- Return replaced carrier ids from duplicate-UUID handling. The Linux relay
  runtime owns the `CarrierId -> TlsTcpCarrierSession` map and stops replaced
  sessions.
- Pass inbound decoded frames to the TUN adapter by `CarrierId`, not by session
  pointer.
- Split CMake targets so `fps_tun_adapter` links the generic datagram core, not
  the concrete TLS/TCP carrier target.

## Follow-Up Increments

- Replace `TunRuntime::run_ip_command(...)` with semantic platform operations,
  keeping Linux `ip` argv generation in the Linux runtime only.
- Extract profile/config parsing that Android must share: `fps://v1`, UUID
  validation, server-public-key validation and normalized non-secret profile
  output.
- Define the Android `VpnService` design: TUN fd ownership, `protect()` for
  carrier sockets, DNS/route behavior, lifecycle/reconnect and status reporting.
- Make carrier enqueue executor affinity explicit, preferably by posting enqueue
  requests onto the session executor or documenting and testing a single
  executor-only contract.

