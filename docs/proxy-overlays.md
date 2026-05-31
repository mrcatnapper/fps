# Proxy Overlays Over FPS

FPS core provides an authenticated covert datagram transport. The current Linux
product adapter exposes those datagrams as a leased L3 TUN VPN service.
Application proxy daemons are deployment overlays on top of that TUN adapter,
not part of the base `fps` image or FPS protocol contract.

The common pattern is:

1. Run `fps_server` with a server-side TUN address, for example `10.66.0.1`.
2. Run a proxy daemon in the same network namespace as `fps_server`.
3. Bind the proxy to the server TUN address.
4. Configure client applications to use the proxy address after `fps_client`
   receives its lease, for example `socks5h://10.66.0.1:1080`.

This avoids client-side split/full route setup for applications that already
support a proxy setting. It does not replace carrier creation: the user still
needs browser/application carrier sessions or the debug `fps_carrier` utility.

## Dante SOCKS5

Dante is the official Docker proxy overlay example because SOCKS5 is broadly
supported and can proxy arbitrary TCP connections with remote DNS through
`socks5h://`.

Build the base FPS image and the derivative Dante image:

```sh
docker build -t fps:local .
docker build -f examples/docker/proxy-dante/Dockerfile \
  --build-arg FPS_BASE_IMAGE=fps:local \
  -t fps-dante-proxy:local .
```

Run it alongside the server compose example:

```sh
cd examples/docker/server
docker compose -f compose.yml -f ../proxy-dante/compose.yml up -d
docker compose -f compose.yml -f ../proxy-dante/compose.yml logs -f fps-dante-proxy
```

The overlay service uses `network_mode: "service:fps-server"`, waits for the
server TUN address and starts Dante in the foreground. Defaults:

- `FPS_SOCKS_LISTEN_ADDRESS=10.66.0.1`
- `FPS_SOCKS_PORT=1080`
- `FPS_SOCKS_ALLOWED_CIDR=10.66.0.0/24`
- `FPS_SOCKS_EXTERNAL_INTERFACE=eth0`

Client applications can then use:

```text
socks5h://10.66.0.1:1080
```

The example is intentionally minimal: no SOCKS auth, no per-user ACLs, no UDP
ASSOCIATE and no traffic shaping. Bind it only to the FPS server TUN address,
keep `FPS_SOCKS_ALLOWED_CIDR` as narrow as possible, and treat it as a working
deployment pattern that operators can fork and harden.

Basic smoke checks from the client side after a lease and at least one carrier
are active:

```sh
curl --socks5-hostname 10.66.0.1:1080 https://api.ipify.org?format=json
curl --socks5-hostname 10.66.0.1:1080 https://www.example.com/
```

Expected signals:

- public IP checks show the server-side public IP;
- ordinary HTTPS GETs return HTTP 200;
- optional POST/upload checks against an operator-controlled HTTPS endpoint
  complete without carrier drops;
- Dante logs show accepted SOCKS connections from the leased client address,
  for example `10.66.0.2`, and outbound TCP connects from the server namespace.

## Squid HTTP Proxy

Squid is a reasonable HTTP/HTTPS CONNECT overlay for applications that support an
HTTP proxy but not SOCKS5. FPS does not ship an official Squid compose example
because SOCKS5 is more general, but the topology is the same: run Squid in the
`fps_server` network namespace and bind it to the server TUN address.

Minimal Squid policy shape:

```text
http_port 10.66.0.1:3128
acl fps_tun src 10.66.0.0/24
http_access allow fps_tun
http_access deny all
```

Clients would configure:

```text
http://10.66.0.1:3128
```

Squid policy, logging, cache behavior and TLS interception settings are outside
the FPS product contract. Avoid transparent proxying until routing and firewall
rules are explicitly reviewed.
