# Linux Routing And DNS Workflow

This document describes the explicit Linux operator workflow around the current
FPS TUN adapter. The reusable FPS core carries opaque datagrams over
authenticated carrier sessions; the Linux VPN adapter maps those datagrams to
IP packets from TUN devices. `fps_client` and `fps_server` open TUN devices and
move IP packets, but they do not silently change host routes, DNS, forwarding,
NAT or firewall policy. Those actions remain visible and reviewable.

For the full Docker-first deployment path, start with
[public-beta-quickstart.md](./public-beta-quickstart.md). This document focuses
on the routing and DNS pieces referenced by that quickstart.

## Helper

`tools/fps_linux_route.sh` can print a plan, apply it and clean up the same
rules:

```sh
tools/fps_linux_route.sh plan --tun fpsc0 --tun-address 10.66.0.2/30 \
  --route 10.66.1.0/24 --dns 10.66.0.1 --dns-domain '~fps.test'

sudo tools/fps_linux_route.sh apply --tun fpsc0 --tun-address 10.66.0.2/30 \
  --route 10.66.1.0/24 --dns 10.66.0.1 --dns-domain '~fps.test'

sudo tools/fps_linux_route.sh cleanup --tun fpsc0 --tun-address 10.66.0.2/30 \
  --route 10.66.1.0/24 --down
```

`plan` and `--dry-run` do not modify the host. `apply` and `cleanup` require
root. DNS is configured through `resolvectl`; pass `--no-dns` when DNS routing is
not needed.

## Carrier Host Override

For browser-created carrier sessions, the recommended beta workflow is an
explicit hosts-file override for the carrier origin name. Example:

```sh
echo '127.0.0.1 carrier.example.net' | sudo tee -a /etc/hosts
```

Then point the browser or carrier-capable application at the ordinary origin URL:

```text
https://carrier.example.net/
wss://carrier.example.net/
```

The browser still uses `carrier.example.net` as the origin, SNI and `Host`
header, while Linux connects to the local `fps_client` listener. This is usually
the simplest way to avoid certificate and CORS failures that happen when a
browser is sent to `https://127.0.0.1/...` for a site configured for another
hostname.

Important constraints:

- DNS and `/etc/hosts` map names to IP addresses, not ports. Use local port
  `443` for default HTTPS/WSS URLs, or open an explicit URL with the FPS client
  port.
- Add an `::1` entry only if the local FPS client listens on IPv6; otherwise
  IPv6-capable clients can bypass the IPv4 hosts entry.
- Browser DNS-over-HTTPS or application-internal resolvers can bypass system
  hosts resolution and must be disabled or configured separately.
- FPS does not provide a DNS proxy. Use explicit hosts entries or router DNS
  overrides for selected carrier domains.

On a home router or OpenWrt-style LAN gateway, the same idea can be applied once
at the LAN DNS layer instead of on every client device. Configure the router DNS
service to return the router LAN IP for selected carrier hostnames, then run
`fps_client` on that router address and port. LAN applications still open the
ordinary carrier URL, while TCP reaches the router-hosted FPS client. This is a
documented deployment pattern, not yet a tested release target; verify container
runtime support, CPU architecture, `/dev/net/tun`, `NET_ADMIN`-equivalent
permissions and port binding on the router first.

## Split Tunnel

Split tunneling is the recommended baseline for early production-like runs:

```sh
sudo ip addr replace 10.66.0.2/30 dev fpsc0
sudo ip link set dev fpsc0 mtu 1280
sudo ip link set dev fpsc0 up
sudo ip route replace 10.66.1.0/24 dev fpsc0
sudo resolvectl dns fpsc0 10.66.0.1
sudo resolvectl domain fpsc0 '~fps.test'
```

Cleanup:

```sh
sudo resolvectl revert fpsc0
sudo ip route del 10.66.1.0/24 dev fpsc0 2>/dev/null || true
sudo ip addr del 10.66.0.2/30 dev fpsc0 2>/dev/null || true
sudo ip link set dev fpsc0 down
```

The server side needs the mirrored address and route, for example
`10.66.0.1/30` on `fpss0`, plus a route to any split subnet that is not directly
on the TUN link.

## Full Tunnel

Full tunnel mode is riskier because the FPS carrier TCP connection can
accidentally be routed into the FPS TUN itself. For that reason the helper
requires an explicit policy selector: `--fwmark` or `--from`.

Example policy table for traffic that is already marked with `0x465053`:

```sh
sudo tools/fps_linux_route.sh apply --tun fpsc0 --full-tunnel \
  --table 100 --priority 10000 --fwmark 0x465053 \
  --bypass 203.0.113.10/32,192.0.2.1,eth0
```

`--bypass` adds a route to the FPS server/carrier endpoint through the underlay
so control/carrier TCP does not loop into the TUN. Packet marking should be done
separately and explicitly, for example through nftables rules for a dedicated
Unix user or cgroup. Minimal example for a dedicated `fps-app` user:

```sh
sudo nft add table inet fps_mark
sudo nft 'add chain inet fps_mark output { type route hook output priority mangle; policy accept; }'
sudo nft add rule inet fps_mark output meta skuid fps-app meta mark set 0x465053
```

Cleanup:

```sh
sudo nft delete table inet fps_mark
sudo tools/fps_linux_route.sh cleanup --tun fpsc0 --full-tunnel \
  --table 100 --priority 10000 --fwmark 0x465053 \
  --bypass 203.0.113.10/32,192.0.2.1,eth0
```

Internet egress through the server also requires server-side IP forwarding and
NAT/masquerade. The helper intentionally does not automate firewall/NAT policy:
those rules are deployment-specific and should be applied by a separate reviewed
playbook.

## Safety Notes

- Run `plan` before `apply` and verify that the carrier/server route stays on
  the underlay.
- The helper does not modify the global default route unless `--full-tunnel` is
  explicitly requested.
- The helper does not create nftables/iptables rules, enable forwarding or
  change NAT.
- The current workflow is IPv4-first. IPv6 routing should be added only after
  the address/lease model is reviewed.
