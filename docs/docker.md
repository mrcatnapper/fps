# Docker Runtime

FPS ships a single Linux Docker image that contains `fps_client`, `fps_server`,
`fps_linux_route.sh`, `fps_carrier` and a small entrypoint. The image runs the
current product adapter: a leased L3 TUN VPN service built on top of the generic
FPS covert datagram transport. It is intended for server-first deployments and
for client deployments where the operator accepts the Linux TUN/capability
requirements.

For the end-to-end public beta operator flow, start with
[public-beta-quickstart.md](./public-beta-quickstart.md). This document is the
runtime reference for Docker image behavior, compose examples and troubleshooting
details.

For browser or application carriers against a real external origin, see
[real-origin-carriers.md](./real-origin-carriers.md). `fps_carrier` remains the
deterministic debug path; real-origin flows are useful for operator smoke tests
that do not depend on FPS-specific carrier tooling.

The same real-origin DNS override can be centralized on a LAN router. In that
pattern the router runs `fps_client`, listens on its LAN address, and serves DNS
answers that map selected carrier hostnames to that LAN address. This can work
on OpenWrt-style systems only when the router has suitable CPU architecture,
container/runtime support, TUN support and the needed network capabilities; it
is not yet part of the tested release matrix.

Proxy daemons are intentionally not part of the base FPS image. If an operator
wants an application-level proxy on top of the FPS TUN adapter, use a small
overlay image such as the official Dante example in
`examples/docker/proxy-dante`. DHCP is still deferred because the current VPN
adapter is L3 TUN; ordinary DHCP expects L2/broadcast semantics.

## Build

```sh
docker build -t fps:local .
```

For the smaller production-oriented Alpine image:

```sh
docker build -f Dockerfile.alpine -t fps:alpine .
```

The Alpine build uses distro packages for Boost/OpenSSL/iproute2/iperf3 and a
pinned musllinux wheel for `websockets==12.0`; it does not build runtime
dependencies from source. The Alpine image currently supports the GCC builder
path only.

The compose examples default to `Dockerfile`, but can build the Alpine image
without editing YAML:

```sh
FPS_DOCKERFILE=Dockerfile.alpine docker compose build
```

If Docker access requires root in the current environment, use `sudo docker` or
run the project quality hook as:

```sh
FPS_DOCKER_SUDO=1 tools/run_quality_checks.sh --docker
```

On weak servers, build images on a stronger local machine and load them over SSH
instead of compiling on the server:

```sh
docker build -t fps:local .
docker build -f examples/docker/proxy-dante/Dockerfile \
  --build-arg FPS_BASE_IMAGE=fps:local \
  -t fps-dante-proxy:local .
docker save fps:local fps-dante-proxy:local | ssh fps.example.net docker load
```

After loading, copy only configs, compose files and runtime secrets to the
server, then run the same compose commands there.

Both Dockerfiles are multi-stage: the builder image installs the C++ toolchain
and Boost/OpenSSL headers, while the runtime image keeps installed FPS binaries,
`iproute2`, `iperf3`, `ca-certificates`, `python3`, `openssl`, the pinned
`websockets` runtime dependency and shared runtime libraries. `iperf3` is kept
as an operator/debug smoke tool for TUN throughput checks, not as a protocol
dependency.

## Traffic-Shape Fidelity

Docker remains the primary deployment path for beta operators, but it is not a
perfect packet-capture laboratory. Docker bridge, veth, VM and host offload
layers can expose coalesced 8-14 KiB packets to endpoint `tcpdump` through
GRO/GSO/TSO or hypervisor aggregation. Those captures are useful for TLS
byte-stream validation and coarse shaper regressions; they do not prove physical
wire packet sizes.

For stricter traffic-analysis experiments, prefer either native binaries or
Docker host networking, and capture on the external interface or an external
tap. If measuring on the endpoint, explicitly account for offload settings.
The FPS relay sets `network.tcp_no_delay=true` by default on its TCP sockets so
the shaper, not Nagle, controls classified-record timing.

## Entrypoint API

Default command:

```sh
docker run --rm fps:local
```

Environment:

- `FPS_ROLE=server|client`, default `server`.
- `FPS_CONFIG=/etc/fps/server.json`, role-specific config path.
- `FPS_LOG_LEVEL=info|debug|...`, optional CLI override.
- `FPS_CONFIGURE_TUN=1`, opt-in TUN address/route setup inside the container
  namespace.
- `FPS_TUN_NAME=fpss0`, required when `FPS_CONFIGURE_TUN=1`.
- `FPS_TUN_ADDRESS=10.66.0.1/30`, optional address to apply.
- `FPS_TUN_MTU=1280`, optional MTU.
- `FPS_TUN_ROUTES=10.66.1.0/24,10.66.2.0/24`, optional comma-separated routes.

When `FPS_CONFIGURE_TUN=1`, the entrypoint starts FPS, waits for the configured
TUN interface to appear, then applies the address, MTU and routes. This is
needed because the FPS process creates the TUN device.

Explicit commands bypass the role runner, which is useful for key/config tooling:

```sh
docker run --rm fps:local fps_client --help
docker run --rm fps:local fps_carrier --help
docker run --rm fps:local fps_client --generate-client-uuid
docker run --rm fps:local fps_server --generate-server-keypair --format json
```

Two short aliases use `FPS_ROLE` and `FPS_CONFIG` instead of requiring the
binary name:

```sh
docker run --rm -e FPS_ROLE=server -v "$PWD/config:/etc/fps:ro" fps:local check-config
docker compose run --rm --no-deps fps-server status
docker compose run --rm --no-deps fps-client status
```

## Server Compose

`examples/docker/server/compose.yml` runs the FPS server in a normal Docker
bridge network and publishes the carrier port:

```sh
cd examples/docker/server
docker compose build
docker compose up -d
docker compose logs -f fps-server
```

The example publishes container port `8443` by default. To bind the public host
port `443` without editing YAML:

```sh
FPS_PUBLISHED_PORT=443 docker compose up -d
```

Validate the public carrier port before debugging FPS authentication:

```sh
# On the server host.
ss -ltnp | grep ':443'
docker compose ps

# From the client network.
nc -vz fps.example.net 443
```

If `docker compose ps` shows `0.0.0.0:443->8443/tcp` but the client-side `nc`
probe times out, check the provider firewall or security group. High ports can
be blocked even when Docker published them correctly; the beta examples assume
public HTTPS `:443` unless the deployment explicitly opens another port.

Before start, generate a server key pair, paste
`server_private_key_base64`/`server_public_key_base64` into the mounted JSON, put
client UUIDs into `allowed_client_uuids`, and edit
`examples/docker/server/config/server.json` for the public carrier endpoint,
origin endpoint, profile id and TUN lease pool. The server persists non-secret
public-key-to-IP leases under `/var/lib/fps/leases.json`. The compose example
grants:

- `/dev/net/tun`
- `cap_add: [NET_ADMIN, NET_BIND_SERVICE]`
- no `privileged: true`

Optional application proxy overlays are documented separately in
[proxy-overlays.md](./proxy-overlays.md). The official Docker example is a Dante
SOCKS5 overlay image that shares the `fps-server` network namespace and listens
on the server TUN address.

## Runtime Status

The example configs enable `ops.status_socket` under `/run/fps` and share that
directory through a named runtime volume. Query it with the role-aware entrypoint
alias:

```sh
docker compose run --rm --no-deps fps-server status
docker compose run --rm --no-deps fps-client status
```

`docker compose exec ... fps_server --status ...` still works when you prefer to
query from inside the running daemon container.

The command prints one JSON snapshot with role, pid, uptime, session/carrier
counters, auth and classified-record counters, recent close metadata, TUN
packet/drop counters and shaper/backpressure counters. `auth.authenticated`,
`sessions.carriers_current`, `classified_record.decode_failed` and
`classified_record.encode_failed` are the first fields to check when a carrier
does not behave as expected. `sessions.last_closed` and
`sessions.recent_closed` explain the last bounded set of close reasons, such as
peer EOF, target connect failure, classified-record decode failure or write queue
saturation. It is a local read-only health surface, not a management API. It
never prints UUIDs, keys or payload bytes.

`write_queue_full` means FPS could not enqueue more bytes for that carrier
without exceeding the configured per-session queue budget. For TUN packets this
rejects/drops the packet at the FPS boundary and increments exact status
counters; for authenticated cover bytes the carrier is closed because FPS cannot
silently discard the browser-origin TCP stream. Repetitive log lines are
aggregated at roughly ten-second intervals, so use status counters for exact
totals.

## Client Host-Network Compose

`examples/docker/client-host/compose.yml` uses `network_mode: host`. This is the
simplest way for a containerized FPS client to create a TUN interface and routes
in the host network namespace:

```sh
cd examples/docker/client-host
docker compose build
docker compose up -d
docker compose logs -f fps-client
```

Host-network mode means route changes affect the host. Start with split routes
and review `tools/fps_linux_route.sh plan` before adding full-tunnel behavior.
With `tun.auto_configure=true`, the FPS client applies the server-assigned TUN
address after Zero-RTT authentication, and Linux creates the connected route for
that TUN prefix. `FPS_TUN_ADDRESS` and `FPS_TUN_ROUTES` are intentionally absent
from the default client example; add extra split/full routes only after reviewing
the route helper plan.

For browser-created carrier sessions, map the carrier hostname to the local
client listener instead of opening a `127.0.0.1` URL:

```sh
echo '127.0.0.1 carrier.example.net' | sudo tee -a /etc/hosts
```

Then open `https://carrier.example.net/` or `wss://carrier.example.net/`. This
keeps the browser origin, SNI and `Host` header aligned with the real carrier
origin and avoids the usual certificate/CORS errors caused by changing the
visible URL to loopback. DNS cannot change ports, so default HTTPS/WSS URLs
require the local FPS client to listen on port `443`, or the user must open an
explicit URL containing the configured local port.

`tun.leased_client_address` in client status means the client authenticated and
received an address. Traffic still requires `sessions.carriers_current > 0`.
When carriers close, routes and SOCKS settings can be correct while the tunnel
has no live cover session. See [real-origin-carriers.md](./real-origin-carriers.md)
for persistent WebSocket carrier examples.

Administrators can generate a client JSON profile from the server config instead
of hand-editing the client file:

```sh
docker run --rm -v "$PWD/config:/etc/fps:ro" fps:local \
  fps_server --generate-client-profile \
  --config /etc/fps/server.json \
  --client-uuid "$CLIENT_UUID" \
  --server-endpoint fps.example.net:8443 > client.json
chmod 600 client.json
```

Use `--format uri` to print the same profile as a single `fps://v1/...` string.
Inside the image, `fps_client --print-config-from-uri 'fps://v1/...'` decodes it
back to JSON for inspection or mounting. To avoid shell redirection and create a
secret config file directly:

```sh
docker run --rm -v "$PWD/config:/etc/fps" fps:local \
  fps_client --write-config-from-uri 'fps://v1/...' \
  --output /etc/fps/client.json
```

Profile output helpers write files with `0600` permissions and refuse to
overwrite existing paths unless `--force` is set. If these helpers are run from
a root-running Docker container against a bind mount, the resulting host file is
root-owned. Prefer one of these explicit ownership patterns, and do not write
`--output /tmp/client.json` in an unmounted one-shot container because that file
will disappear when the container exits:

```sh
# Host-owned file through host redirection.
docker run --rm -v "$PWD/config:/etc/fps:ro" fps:local \
  fps_server --generate-client-profile --config /etc/fps/server.json \
  --client-uuid "$CLIENT_UUID" --server-endpoint fps.example.net:8443 \
  > client.json
chmod 600 client.json

# Host UID/GID for direct bind-mount writes.
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD/config:/etc/fps" fps:local \
  fps_client --write-config-from-uri 'fps://v1/...' \
  --output /etc/fps/client.json
```

Explicit `sudo chown "$USER:$USER" config/client.json` is also acceptable when
the operator deliberately writes from a root-running container.

## Debug Carrier

`fps_carrier` is a debug HTTPS/WSS echo carrier generator built on the pinned
`websockets` Python package. It is useful when an operator wants FPS to have a
continuous cover session or a simple browser-visible origin without arranging a
separate application:

```sh
docker run --rm fps:local fps_carrier origin \
  --listen 0.0.0.0:9443 \
  --cert /tmp/fps-carrier/cert.pem \
  --key /tmp/fps-carrier/key.pem \
  --generate-self-signed

docker run --rm fps:local fps_carrier client \
  --connect 127.0.0.1:7443 \
  --client-bps 4096 \
  --frame-rate 4
```

The generator maintains a long-lived TLS WebSocket connection, reconnects by
default, and validates deterministic binary echo frames. The same origin also
answers ordinary HTTPS `GET /` requests with a small text response, so an
operator can create browser carrier sessions without a separate origin service.
It is a debug and regression utility, not a traffic-analysis-resistant shaper
profile.

For Docker-first deployments, run `fps_carrier origin` next to `fps_server` and
point `fps_server` at that origin. Run `fps_carrier client` next to `fps_client`
or open the origin URL in a browser through FPS. Use an operator-controlled
carrier origin, or a real application origin that the operator is already
legitimately expected to use, for repeatable tests and user-flow checks.

`examples/docker/debug-carrier/compose.yml` runs four separate services from the
same image: carrier origin, `fps_server`, `fps_client` and carrier client. This
example intentionally does not enable TUN; it verifies the FPS carrier chain.
For TUN deployments, run carrier origin/client services next to the server/client
compose examples and keep FPS TUN setup in the FPS services.

With `tun.auto_configure=true`, the client applies the server-assigned TUN
address and MTU. Linux naturally adds the connected route for that TUN prefix;
DNS, default routes, split routes and firewall/NAT policy remain explicit
operator choices.

## TUN UDP Iperf Simulation

`tools/docker_tun_iperf_sim.py` creates an ephemeral compose project with four
services: `fps_server`, `fps_client`, `fps_carrier origin` and `fps_carrier
client`. It generates a temporary server key pair and UUID config, assigns
`10.88.0.1/30` on the server side, waits for the server to lease `10.88.0.2/30`
to the client, then runs UDP `iperf3` over the hidden TUN path:

```sh
FPS_DOCKER_SUDO=1 tools/docker_tun_iperf_sim.py --build \
  --duration 10 --bandwidth 250K --length 512
```

The script prints JSON with UDP throughput, packet loss and a check that FPS
`event=session_stats` logs contain non-zero TUN frame counters. It intentionally
keeps routing inside Docker namespaces, so host routes/firewall policy are not
changed.

## Multi-Client Lease Simulation

`tools/docker_multi_client_sim.py` creates an ephemeral compose project with one
server, two UUID-backed clients and two WSS carrier clients. Both clients receive
distinct server-assigned leases from `10.89.0.0/29`. The script runs UDP probes
from server to each client lease and from each client back to the server, sends a
single spoofed-source UDP packet from one client, waits for
`event=ignored_spoofed_tun_source`, then proves valid leased traffic still works:

```sh
FPS_DOCKER_SUDO=1 tools/docker_multi_client_sim.py --build
```

This is the main product-level regression for multi-client lease routing. It
does not change host routes or firewall policy.

## Duplicate UUID Replacement Simulation

`tools/docker_duplicate_uuid_sim.py` starts one server and two clients with the
same UUID in a staged compose flow. The first client receives a lease, the second
client authenticates with the same lease and the server emits
`event=duplicate_client_replaced`. The script then stops the first carrier
helper, verifies that server-to-client traffic is no longer delivered to the old
client and verifies that the newer client remains functional:

```sh
FPS_DOCKER_SUDO=1 tools/docker_duplicate_uuid_sim.py --build
```

The status check asserts a non-secret `duplicate_client_replacements` counter
without exposing UUIDs, keys or raw client instance ids.

## Dante Proxy Overlay Smoke

`tools/docker_socks_smoke.py` creates a similar ephemeral compose project and
adds the official derivative Dante proxy image plus a simple HTTP origin. The
FPS client opens a SOCKS5 TCP connection to `10.88.0.1:1080` over TUN and
requests the HTTP origin through Dante:

```sh
FPS_DOCKER_SUDO=1 tools/docker_socks_smoke.py --build
```

The script keeps all routes inside Docker namespaces and prints JSON containing
the SOCKS probe result and service liveness.

## Config Layout

Recommended mount layout:

```text
/etc/fps/
  server.json or client.json
```

Config files are mounted read-only in the examples. Server configs contain
`server_private_key_base64`, so treat the mounted JSON as secret material. Client
UUIDs are bearer secrets and should be stored with the same care as passwords or
private keys. User-facing auth config intentionally supports only client UUIDs
and inline server base64 keys; raw key-file modes are rejected by `--check-config`.
Issue one UUID per device/profile. Sharing one UUID across several machines is
unsupported because those machines would receive the same persistent lease
identity; the enforced duplicate policy is `replace_old`, where a newer
active instance supersedes older carriers for that UUID. The per-process client
instance id used to make that decision is encrypted runtime metadata and is not
part of the config/profile schema.

## Troubleshooting

- `cannot open /dev/net/tun`: pass `devices: ["/dev/net/tun:/dev/net/tun"]` and
  ensure the host has TUN support loaded.
- `operation not permitted`: add `NET_ADMIN`; low carrier ports also need
  `NET_BIND_SERVICE`.
- `timed out waiting for TUN interface`: `tun.name` in JSON must match
  `FPS_TUN_NAME`.
- `client did not apply server-assigned lease`: confirm the server has
  `tun.lease_pool`/`tun.lease_file`, the UUID is allowlisted, and the client has
  `tun.auto_configure=true`.
- `target_connect_failed`: confirm `network.server`/`network.origin` points to a
  reachable endpoint from inside the FPS container namespace. In bridge-mode
  Docker, 127.0.0.1 is container loopback, not host loopback. The primary client
  Docker example uses `network_mode: host` to avoid this ambiguity.
- `no_carrier_session`: the TUN adapter received traffic before any carrier
  authenticated. Open or restart the browser/application/debug carrier and wait
  for `sessions.carriers_current > 0`.
- `non_ipv4_tun_destination` or `ignored_non_ipv4_tun_packet`: usually benign
  host/container network noise in minimal IPv4 deployments. Treat it as a
  problem only if counters grow together with user-visible packet loss.
- `SOCKS listener did not become reachable`: check the Dante overlay logs and
  ensure `FPS_SOCKS_LISTEN_ADDRESS` matches the server TUN address.
- `tun.leased_client_address` exists but `sessions.carriers_current=0`: the
  client authenticated earlier, but all carriers are now closed. Restart or
  recreate carrier sessions before changing routes or proxy settings.
- `multi-client smoke did not receive distinct leases`: inspect `fps-server`
  logs for Zero-RTT auth failures and stale `/var/lib/fps/leases.json` in kept
  artifacts.
- `--check-config` failures happen before the daemon starts; fix mounted config
  values first.
