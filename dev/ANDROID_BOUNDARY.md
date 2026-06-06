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
  datagram core. The current TLS/TCP carrier and TUN adapter are explicit
  opt-in targets above it.
- `TunRuntime` is injectable, but its current command surface is Linux-shaped:
  it exposes `run_ip_command(...)`. Android will need semantic link/address
  operations backed by `VpnService`, not `ip` command arguments.
- Runtime enqueue calls are effectively same-executor operations. Android JNI or
  Kotlin callbacks must not call into session queues from arbitrary threads
  until the enqueue contract is made explicit.

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
