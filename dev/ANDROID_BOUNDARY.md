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

## Accepted Android Direction

- First Android beta should use app-owned carrier sessions. The Android app
  opens and maintains the cover connections itself instead of relying on a
  browser/game/third-party app to create them.
- Carrier behavior should be app-configurable at the Android layer: for example
  periodic HTTPS GETs, TCP keepalive-friendly requests or WSS stream probes. This
  does not require protocol-core changes yet.
- Add a platform socket-protection hook before Android carrier `connect`. Linux
  remains no-op; Android calls `VpnService.protect(fd)` before the socket can be
  captured by the VPN.
- Resolve FPS server/carrier hostnames through Android's underlying
  `ConnectivityManager.Network`, then pass resolved endpoints into native code.
  Do not rely on native resolver behavior after VPN activation.
- Use two-phase TUN startup: authenticate and receive server lease first, then
  create/configure the Android `VpnService` fd and start the native TUN pump.
- Android default route mode is split tunnel. Full tunnel is an explicit
  advanced option.
- Keep the C++ core synchronous and same-executor. Android should expose a thin
  async/JNI facade that posts all native operations onto the FPS `io_context`.

## Alternatives Kept For Future Review

- Third-party/browser carrier sessions remain useful for a later advanced mode,
  but they reopen DNS mapping and VPN-loop risks. Android has no simple
  `/etc/hosts` equivalent, so that mode likely needs a controlled DNS proxy or
  per-app routing design.
- A local DNS proxy that maps selected carrier origins to FPS may help external
  carrier UX later, but it should not be part of the first Android beta. It must
  avoid resolving FPS's own carrier sockets back into the VPN.
- Passing already-protected Java/Kotlin sockets or fds into native code is a
  possible alternative to a C++ protector hook, but it complicates Boost.Asio
  ownership, resolver behavior and connect lifecycle.
- Creating a dummy VPN before lease assignment is possible, but it risks
  dropping user traffic during auth and complicates reconnect. The accepted
  model is to start TUN only after a lease is available.
- A fully async C++ carrier/datagram API can be reconsidered if JNI facade
  posting becomes too limiting. Mutex-protecting session queues is not a good
  alternative because it hides ordering and deadlock risks.
