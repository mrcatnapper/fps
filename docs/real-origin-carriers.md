# Real-Origin Carrier Sessions

FPS can use ordinary browser or application TLS sessions as carriers. The debug
`fps_carrier` utility is useful for deterministic tests, but beta operators also
need to understand the real-origin flow.

## Core Rule

The application must still believe it is connecting to the real carrier origin.
Only the TCP destination is redirected to the local `fps_client` listener.

That means:

- the visible URL stays the real origin, for example
  `wss://carrier.example.net/stream`;
- TLS SNI and `Host` stay the real hostname;
- the local TCP endpoint is `127.0.0.1:443` or whichever port `fps_client`
  listens on;
- `/etc/hosts` is the simplest beta mechanism for browser/app flows.

Do not open `https://127.0.0.1/` for a hostname-based origin. That changes the
browser origin and usually breaks certificates, SNI, `Host`, cookies or CORS.

## Server Placement

For a self-hosted carrier origin, `fps_server` should be treated as a
pre-reverse-proxy: it is the public TCP gate in front of the real origin path,
but it must not terminate the carrier TLS session itself.

A typical deployment can be:

```text
Internet
  -> fps_server
  -> optional L4/TCP proxy, TLS passthrough load balancer or SNI router
  -> real reverse proxy that terminates TLS
  -> backend application
```

The real reverse proxy, application gateway or backend can terminate TLS only
after `fps_server` has already relayed the carrier byte stream. Ordinary HTTP
reverse proxies or CDNs that terminate TLS before the traffic reaches
`fps_server` break this model, because FPS no longer sees the same end-to-end
carrier TLS records.

L4/TCP proxying, TLS passthrough and SNI-routing products can fit this topology
because they forward TCP/TLS without decrypting it. They are less common than
standard HTTP reverse-proxy/CDN setups and may have different operational or
pricing tradeoffs, so treat this as an explicit deployment requirement rather
than an automatic property of "put it behind a proxy".

## Hosts Mapping

For a default HTTPS/WSS origin on port `443`:

```sh
echo '127.0.0.1 carrier.example.net' | sudo tee -a /etc/hosts
```

Then connect to the original URL:

```text
wss://carrier.example.net/stream
```

DNS and `/etc/hosts` do not rewrite ports. If the local FPS client listens on a
non-default port, use a tool that can connect to that port while preserving SNI
and `Host`, or change the client listener to `127.0.0.1:443`.

Remove the mapping after the test:

```sh
sudo sed -i '/carrier.example.net/d' /etc/hosts
```

## Router Or LAN Gateway Pattern

A router can act as the FPS client entry point for a whole LAN. This is useful
when the operator wants applications on laptops, phones or TVs to create carrier
sessions without running `fps_client` on each device.

Conceptual shape:

```text
LAN device opens wss://carrier.example.net
        |
        v
router DNS maps carrier.example.net -> router LAN IP
        |
        v
fps_client listens on router LAN IP:443
        |
        v
fps_server -> carrier origin
```

The same core rule still applies: the LAN application must use the real carrier
hostname in its URL, SNI and `Host` header. Only DNS resolution changes.

Router-side requirements:

- Linux TUN support and permission to create a TUN device.
- A way to run the FPS client image or binary on the router CPU architecture.
  OpenWrt can act as a container host on supported targets, but this FPS
  deployment mode is not yet part of the tested release matrix.
- `fps_client` must listen on a LAN-reachable address and port, commonly the
  router LAN IP on `:443`; binding to `127.0.0.1` would only work for processes
  running on the router itself.
- Router DNS must return the router LAN IP for selected carrier hostnames and
  keep ordinary DNS fallback for everything else.
- Carrier applications still need to stay open somewhere on the LAN. Examples:
  a WebSocket tab, a game client, an internet radio client or another
  long-lived TLS application session to the chosen origin.

For OpenWrt-style DNS, the usual pattern is a per-domain `dnsmasq`/DHCP-DNS
override through LuCI or UCI rather than editing every LAN device. Exact syntax
depends on the router image and DNS stack, but the intent is:

```text
carrier.example.net -> 192.168.1.1
```

where `192.168.1.1` is the router LAN address that `fps_client` listens on.

This pattern does not remove the live-carrier requirement. LAN clients can have
working DNS and a valid FPS lease while the tunnel is unusable because
`sessions.carriers_current == 0`. Keep at least one carrier session alive before
debugging SOCKS, routes or application traffic.

## Readiness Semantics

A leased TUN address does not mean the tunnel is usable by itself. It means the
client identity authenticated and received an address. VPN/SOCKS traffic also
needs at least one live carrier.

Check both sides:

```sh
docker compose run --rm --no-deps fps-server status
docker compose run --rm --no-deps fps-client status
```

Ready signals:

- `auth.authenticated > 0`;
- `sessions.carriers_current > 0`;
- client status has `tun.leased_client_address`;
- TUN packet counters increase while traffic crosses the tunnel;
- `auth.*_failed` and `envelope.*_failed` counters stay at zero for a clean
  smoke run.

If `tun.leased_client_address` exists but `sessions.carriers_current == 0`, the
client has an address but no current cover session. Open or restart a carrier
client/browser tab before debugging routes or SOCKS.

## Persistent WebSocket Carrier

Use an origin and application protocol that the operator controls or is already
legitimately expected to use. Good carrier candidates are long-lived TLS
sessions such as WebSocket streams, HTTPS long polling, keep-alive HTTPS flows,
internet radio clients, game clients or other applications that naturally keep
TLS connections open.

The exact carrier client depends on the chosen origin. For an
operator-controlled WebSocket echo endpoint, a minimal Python holder can keep
several sessions alive after the `/etc/hosts` mapping is in place:

```sh
python3 -m pip install --user websockets==12.0
export FPS_CARRIER_URL='wss://carrier.example.net/stream'
python3 - <<'PY'
import asyncio
import itertools
import os
import ssl
import websockets

URL = os.environ["FPS_CARRIER_URL"]

async def hold(index):
    ssl_context = ssl.create_default_context()
    async with websockets.connect(URL, ssl=ssl_context) as ws:
        for counter in itertools.count():
            message = f"fps-carrier-{index}-{counter}"
            await ws.send(message)
            echoed = await ws.recv()
            if echoed != message:
                raise RuntimeError(f"unexpected echo: {echoed!r}")
            await asyncio.sleep(1.0)

async def main():
    await asyncio.gather(*(hold(i) for i in range(3)))

asyncio.run(main())
PY
```

For `websocat`, use the real URL after adding the hosts entry:

```sh
websocat -v wss://carrier.example.net/stream
```

Type a message and confirm it is echoed. Keep the process running while testing
TUN or SOCKS traffic. For non-echo origins, use the normal browser, game,
streaming or application client that speaks that origin's real protocol.

## HTTPS Sanity Check

`curl --resolve` is useful for a short carrier sanity check without editing
`/etc/hosts`:

```sh
curl -vk --resolve carrier.example.net:443:127.0.0.1 \
  https://carrier.example.net/
```

This preserves SNI and `Host`, but it is only a one-shot HTTPS request. Some
origins return `404` for non-WebSocket paths or close immediately after the
response. That can authenticate a short carrier, but it will not keep TUN/SOCKS
traffic alive.

## Production Guidance

Prefer an operator-controlled origin for repeatable deployments. FPS
documentation intentionally avoids recommending third-party public test services
as carrier origins: availability, rate limits, acceptable-use policy and protocol
behavior are outside FPS control.
