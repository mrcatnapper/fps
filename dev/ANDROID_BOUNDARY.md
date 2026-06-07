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
- Outbound TCP carrier connects go through `TcpSocketProtector` before
  `connect`. Linux uses a no-op protector; Android can call
  `VpnService.protect(fd)` on the opened native socket before it can be captured
  by the VPN.
- TUN packets read from the platform fd can be filtered before covert enqueue
  through `TunTunnelHandlers::on_outbound_tun_packet`. The hook receives raw
  packet bytes plus an optional parsed IPv4 TCP/UDP 5-tuple for Android
  `ConnectivityManager.getConnectionOwnerUid(...)` split-tunnel enforcement.

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
- Added a reusable TCP socket-protection hook and changed the relay outbound
  connect path to explicitly open, protect and only then connect target sockets.
- Added `parse_ipv4_flow_tuple(...)` and an outbound TUN packet policy hook so
  Android can fail closed for packets whose initiating UID is not in the
  configured split-tunnel allowlist.
- Added the first Android/Kotlin/NDK scaffold. It builds a minimal Android app
  module with a `VpnService` shell, headless split-tunnel policy tests and a
  JNI library that reuses `parse_ipv4_flow_tuple(...)` for `arm64-v8a` and
  `x86_64`.
- Added a headless Android profile/runtime layer that parses carrier probe
  metadata and split-tunnel UID allowlists, exposes carrier runtime plans,
  resolves carrier endpoints through platform hooks and evaluates fail-closed
  UID policy decisions without opening real sockets.
- Added a deterministic headless carrier manager that drives those carrier
  plans with fake-friendly transports, enforces resolve/protect/connect order,
  handles probe failures through bounded reconnect/backoff and exposes
  non-secret carrier status counters.
- Added an OkHttp-backed live carrier transport factory for Android HTTPS GET
  and WSS probes. It keeps the manager contract intact, uses the
  already-resolved underlying-network endpoint for DNS and protects each Java
  `Socket` before connect through the platform hook.
- Added the first Android `VpnService` lifecycle and TUN fd ownership layer:
  profile start, stop action, lease-triggered `VpnService.Builder`
  establishment, leased IPv4 address/route planning and idempotent
  `ParcelFileDescriptor` close through `HeadlessVpnController.stop()`.
- Extended the Android native smoke to compile reusable protocol codec/crypto,
  generic covert datagram transport and TLS/TCP carrier session sources with
  Android OpenSSL and Boost.Asio.
- Confirmed that header-only Boost.Describe/MP11/Endian can stay available for
  Android when exposed through an isolated Boost header root. The failure mode
  was host `/usr/include` leakage into the NDK sysroot, not Boost.Describe
  itself.
- Added an Android `FPS_LOG_*` macro backend over `__android_log_print`; Linux
  keeps Boost.Log behind the same project facade.

## Follow-Up Increments

- Keep Android profile parsing in Kotlin for now. It uses Android `org.json`
  and the current `fps://v1` client profile shape instead of dragging Linux
  daemon/Boost.JSON config paths into the app.
- Wire the established `VpnService` fd into the native FPS TUN pump through a
  JNI/async facade, then add lifecycle/reconnect and status reporting around
  that pump.
- Decide whether Android should keep the same synchronous executor-only
  contract or introduce a separate async adapter for UI/JNI-facing calls.

## Accepted Android Direction

- First Android beta should use app-owned carrier sessions. The Android app
  opens and maintains the cover connections itself instead of relying on a
  browser/game/third-party app to create them.
- Carrier behavior is app-configurable at the Android profile/runtime layer.
  The current headless model supports HTTPS GET and WSS probe metadata plus a
  fake-transport runner and a live OkHttp-backed HTTPS/WSS carrier transport
  factory plus first `VpnService` TUN fd ownership. The next step is native
  auth/pump wiring and Android lifecycle resilience, not changing protocol core.
- Use the platform socket-protection hook before Android carrier `connect`.
  Linux remains no-op; Android native sockets use the fd hook and OkHttp-owned
  sockets use the Java `Socket` hook before the socket can be captured by the
  VPN.
- Resolve FPS server/carrier hostnames through Android's underlying
  `ConnectivityManager.Network`, then pass resolved endpoints into native code.
  Do not rely on native resolver behavior after VPN activation.
- Use two-phase TUN startup: authenticate and receive server lease first, then
  create/configure the Android `VpnService` fd and start the native TUN pump.
  Current code implements fd creation/ownership; native pump startup is next.
- Android default route mode is split tunnel. Full tunnel is an explicit
  advanced option.
- Do not rely only on `VpnService.Builder.addAllowedApplication(...)` for
  anti-leak policy. Android should also install an outbound TUN packet policy:
  parse the TCP/UDP 5-tuple, ask `ConnectivityManager.getConnectionOwnerUid`,
  allow only configured UIDs and drop `INVALID_UID`, unsupported protocols,
  malformed packets and unknown fragments by default.
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
- Synthetic TCP RST for policy-rejected TCP SYN packets and UDP flow-decision
  caching are useful UX/performance improvements, but first Android beta can
  start with simple fail-closed packet drops and add those refinements after
  device testing.
- Passing already-protected Java/Kotlin sockets or fds into native code is a
  possible alternative to a C++ protector hook, but it complicates Boost.Asio
  ownership, resolver behavior and connect lifecycle.
- Creating a dummy VPN before lease assignment is possible, but it risks
  dropping user traffic during auth and complicates reconnect. The accepted
  model is to start TUN only after a lease is available.
- A fully async C++ carrier/datagram API can be reconsidered if JNI facade
  posting becomes too limiting. Mutex-protecting session queues is not a good
  alternative because it hides ordering and deadlock risks.
