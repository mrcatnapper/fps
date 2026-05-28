# Public Beta Quickstart

This is the shortest Docker-first path for a controlled Linux beta deployment.
It uses one FPS server, one Linux client, a carrier origin and an optional Dante
SOCKS5 overlay. The deterministic path uses self-hosted `fps_carrier`; real
deployments can also use browser/application sessions to an external origin as
described in [real-origin-carriers.md](./real-origin-carriers.md). Replace
example addresses and names before using the commands outside a lab.

## 1. Build The Image

```sh
docker build -t fps:local .
```

Optional smaller runtime image:

```sh
docker build -f Dockerfile.alpine -t fps:alpine .
```

## 2. Prepare The Server Config

Generate the server key pair:

```sh
docker run --rm fps:local fps_server --generate-server-keypair
```

Generate one UUID per client device or profile:

```sh
CLIENT_UUID="$(docker run --rm fps:local fps_client --generate-client-uuid)"
printf '%s\n' "$CLIENT_UUID"
```

Edit `examples/docker/server/config/server.json`:

- set `server_private_key_base64` and `server_public_key_base64`;
- add the client UUID to `allowed_client_uuids`;
- set `network.origin` to your self-hosted carrier origin, for example
  `fps-carrier-origin:9443`;
- keep `tun.lease_pool`, `tun.server_address` and `ops.status_socket` explicit.

Validate before starting:

```sh
docker run --rm -v "$PWD/examples/docker/server/config:/etc/fps:ro" fps:local \
  fps_server --check-config --config /etc/fps/server.json
```

## 3. Start Server And Carrier

The server example publishes container port `8443`. For a public beta-like
setup, bind it to host port `443`:

```sh
cd examples/docker/server
FPS_PUBLISHED_PORT=443 docker compose up -d
docker compose ps
docker compose run --rm --no-deps fps-server status
```

For deterministic carrier traffic without an external origin, run
`fps_carrier origin` next to the server. The `debug-carrier` compose file is a
compact end-to-end example; production deployments can copy the same service
into their server compose stack.

```sh
cd examples/docker/debug-carrier
docker compose up -d fps-carrier-origin fps-server
docker compose run --rm --no-deps fps-server status
```

## 4. Generate A Client Profile

Generate a JSON profile from the server config:

```sh
docker run --rm -v "$PWD/config:/etc/fps:ro" fps:local \
  fps_server --generate-client-profile \
  --config /etc/fps/server.json \
  --client-uuid "$CLIENT_UUID" \
  --server-endpoint fps.example.net:443 \
  --client-listen 127.0.0.1:443 \
  --client-status-socket /run/fps/client.status \
  --format json \
  --output /tmp/client.json
```

Or generate a URI for transport:

```sh
docker run --rm -v "$PWD/config:/etc/fps:ro" fps:local \
  fps_server --generate-client-profile \
  --config /etc/fps/server.json \
  --client-uuid "$CLIENT_UUID" \
  --server-endpoint fps.example.net:443 \
  --client-listen 127.0.0.1:443 \
  --format uri
```

On the client, import the URI into a secret config file:

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD/config:/etc/fps" fps:local \
  fps_client --write-config-from-uri 'fps://v1/...' \
  --output /etc/fps/client.json
```

Generated client profiles and `fps://v1` URIs contain the client UUID bearer
secret. Treat them like passwords.

## 5. Start The Client

Use the host-network compose example when the client should create the TUN
interface in the host namespace:

```sh
cd examples/docker/client-host
docker compose up -d
docker compose ps
docker compose run --rm --no-deps fps-client status
```

The server assigns the client TUN address after Zero-RTT authentication. Extra
split/full routes are operator policy and are not pushed by the profile.

## 6. Create Carrier Sessions

For real-origin browser/application carrier sessions, map the carrier hostname
to the local client listener:

```sh
echo '127.0.0.1 carrier.example.net' | sudo tee -a /etc/hosts
```

Then open the normal carrier URL in a browser or carrier-capable application:

```text
https://carrier.example.net/
wss://carrier.example.net/
```

Do not open `https://127.0.0.1/` for a hostname-based origin: that commonly
breaks TLS certificates, SNI, `Host` and CORS behavior. DNS and `/etc/hosts` do
not select ports, so default HTTPS/WSS requires the local FPS client to listen
on `127.0.0.1:443`.

For a deterministic debug carrier process instead of a browser:

```sh
docker run --rm fps:local fps_carrier client \
  --connect 127.0.0.1:443 \
  --path /fps-carrier \
  --client-bps 4096 \
  --frame-rate 4
```

Carrier liveness is required for VPN/SOCKS liveness. A leased TUN address means
the client authenticated and received an address; it does not mean the tunnel is
currently usable if every carrier has closed.

For browser or application carriers against a real origin, see
[real-origin-carriers.md](./real-origin-carriers.md). It covers hostname
mapping, persistent WebSocket carriers, long-lived TLS application sessions and
`curl --resolve` sanity checks without depending on a public test service.

The same carrier-hostname override can be centralized on a LAN router: run
`fps_client` on the router, make it listen on the router LAN address, and
configure router DNS to resolve selected carrier hostnames to that LAN address.
This gives the LAN one FPS entry point, while carrier-capable devices still need
to keep carrier sessions open. See
[real-origin-carriers.md](./real-origin-carriers.md#router-or-lan-gateway-pattern).

## 7. Verify The Tunnel

Check status on both sides:

```sh
docker compose run --rm --no-deps fps-server status
docker compose run --rm --no-deps fps-client status
```

Expected signals:

- `auth.authenticated` and `sessions.carriers_current` are non-zero;
- the client has a leased address on `fpsc0`;
- TUN packet counters increase when traffic crosses the tunnel;
- status JSON contains no UUIDs, private keys or payload bytes.

If `tun.leased_client_address` is present but `sessions.carriers_current` is
zero, open a persistent carrier before debugging routes, DNS or proxy settings.

For application proxy mode, use the official Dante overlay example:

```sh
docker build -f examples/docker/proxy-dante/Dockerfile \
  --build-arg FPS_BASE_IMAGE=fps:local \
  -t fps-dante-proxy:local .

cd examples/docker/server
docker compose -f compose.yml -f ../proxy-dante/compose.yml up -d
```

Applications can then use `socks5h://10.66.0.1:1080` after the client receives
its TUN lease.

Minimal SOCKS smoke checks:

```sh
curl --socks5-hostname 10.66.0.1:1080 https://api.ipify.org?format=json
curl --socks5-hostname 10.66.0.1:1080 https://www.example.com/
```

The Dante overlay example has no SOCKS authentication or per-user ACLs. Keep it
bound to the FPS server TUN address and keep `FPS_SOCKS_ALLOWED_CIDR` as narrow
as the deployment allows.

## 8. Cleanup

Stop containers on each host where compose is running:

```sh
docker compose down -v --remove-orphans
```

For the official Dante overlay, include the overlay file:

```sh
docker compose -f compose.yml -f ../proxy-dante/compose.yml down -v --remove-orphans
```

Remove the hosts entry when it is no longer needed:

```sh
sudo sed -i '/carrier.example.net/d' /etc/hosts
```

For two-host tests, also remove temporary runtime directories on both machines,
then verify that the public carrier port and TUN devices disappeared:

```sh
ss -ltn sport = :443
ip link show fpsc0 2>/dev/null || true
ip link show fpss0 2>/dev/null || true
```

Review [linux-routing.md](./linux-routing.md) before adding split or full-tunnel
routes, and [rotation.md](./rotation.md) before reissuing UUIDs or server keys.
