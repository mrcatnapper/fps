# FPS Documentation

FPS is an experimental Linux-first VPN framework: a hidden L3 TUN tunnel carried
inside live TLS cover sessions. These pages are the public operator and project
documentation for the current beta candidate.

Start with the quickstart if you want to run the stack. Use the reference pages
when you need exact CLI, Docker, routing, protocol or testing details.

## Get Started

- [Public beta quickstart](./public-beta-quickstart.md): shortest Docker-first
  path from server config to a working client.
- [Docker runtime](./docker.md): image behavior, compose examples, status
  checks, debug carrier and troubleshooting.
- [Client profiles and carrier setup](./client-profiles.md): generated JSON
  profiles, `fps://v1` URIs, `/etc/hosts` carrier mapping and UUID policy.
- [Real-origin carrier sessions](./real-origin-carriers.md): how to use ordinary
  browser or application TLS sessions as carriers.

## Operations

- [Linux routing and DNS workflow](./linux-routing.md): explicit split/full
  tunnel commands and safety notes.
- [Proxy overlays over FPS](./proxy-overlays.md): optional application proxies
  such as the official Dante SOCKS5 overlay example.
- [UUID and key rotation](./rotation.md): client revocation, lease cleanup and
  server key rotation.
- [Release checklist](./release.md): private/public beta release-candidate
  preflight.

## Reference

- [Protocol and architecture specification](./specification.md): current
  wire rules, Zero-RTT authentication, classified FPS records and lease
  semantics.
- [Testing and quality workflow](./testing.md): local tests, GitHub Actions,
  Docker simulations, fuzzing and soak checks.
- [TCP flow shape experiment](./pcap-flow-analysis.md): pcap-based packet-size
  and timing analysis before and after FPS upgrade.
- [Beta status](./beta-status.md): what is ready now, known risks and public
  beta gates.

## Project Name

The expanded project name, Free Porn Storage, is intentional misdirection. It is
not a description of the software; it gives the project a noisy, hard-to-search
label. The technical documentation uses the short name, FPS.
