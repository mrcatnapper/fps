# Free Porn Storage: Steganography In The Most Ordinary TLS Traffic

Status: article draft in English

Suggested tags: networking, censorship-resistance, tls, steganography, vpn,
privacy, cpp, ai

There is an old tactical rule for hiding a tree: put it in a forest.

The Internet already has a very large forest. It is made of TLS sessions:
browser tabs, WebSocket streams, keep-alive HTTPS requests, game clients, mobile
apps, dashboards, telemetry, video players, chat windows and thousands of quiet
connections that sit on TCP/443 and look completely boring.

So, if the only final purpose of the Internet is to become a storage layer for
porn, why not use it properly and store the porn in the traffic itself?

That joke is where the project name comes from. Free Porn Storage, or FPS, is
not a porn product. It is a deliberately misleading code name, and a
search-hostile one. The serious description is less meme-friendly: FPS is an
experimental framework for carrying hidden datagrams inside live TLS traffic,
with a Linux TUN VPN adapter as its first product use case.

This article, and FPS as a project, are meant as a technical investigation into
a specific direction in the censorship-circumvention arms race: after censors
become good at recognizing protocols that merely pretend to be TLS, a useful
next question is whether a tunnel can start from a real carrier TLS session,
authenticate late, and only then hide encrypted traffic inside the ongoing byte
stream.

## The Escalation Ladder Of Filtering

Censorship systems usually start with simple controls because simple controls
are cheap, operationally understandable and often effective enough. DNS
filtering can poison a response, hijack it, refuse it or force users onto
resolver infrastructure that lies; IP and CIDR filtering is blunter and causes
more collateral damage, but it works before application protocols even get a
chance to speak. Protocol and port policy raises the cost again: TCP/443 might
pass while UDP/443, UDP/53, WireGuard-like UDP or unfamiliar ports disappear,
which is the point where "just use encryption" stops being a complete answer
because an encrypted protocol still has a transport shape.

The next layers move closer to application behavior. Before Encrypted
ClientHello is widely deployed and usable everywhere, a TLS ClientHello can
expose the requested hostname through SNI, so a middlebox can allow an IP while
resetting connections with selected names. Beyond SNI, a system can inspect the
TLS ClientHello, cipher suite order, extensions, ALPN, HTTP/2 settings, record
sizes, handshake timing, TCP behavior and server responses; if a connection
claims to be Chrome but behaves like a small Go binary with an odd extension
list, that mismatch is itself a signal. Finally, active probing and longer-flow
analysis let a censor connect to suspected servers with crafted inputs or keep
watching beyond connection setup, looking at record boundaries, resets,
timeouts, payload sizes, burst patterns and nested protocol behavior.

![Filtering escalation ladder](assets/censorship-filtering-ladder.svg)

Public measurements and reports on large filtering systems have shown pieces of
this ladder in the wild: DNS interference, IP and whitelist-style routing
policy, SNI inspection, active probing and proxy fingerprinting. The exact
implementation varies by network and time, but the trend is stable. The cheap
checks move from names and addresses toward protocol behavior.

This matters because many circumvention tools were designed for an earlier
phase of the ladder: first making traffic encrypted, then making it
TLS-looking, then adding fallbacks, then adding browser-like TLS fingerprints.
The harder problem is how to avoid being classified by the beginning of the
connection when the beginning is exactly where most classifiers prefer to look.

## What TLS Looks Like On The Wire

TLS has two large pieces: the handshake protocol and the record protocol. RFC
8446 describes the handshake as the part that authenticates peers, negotiates
cryptographic parameters and establishes shared keys, while the record protocol
divides protected traffic into records with small visible headers and opaque
payloads. In TLS 1.3, after the handshake, application data is carried in
records whose outer content type is `application_data`, value `23`.

From a middlebox viewpoint, the beginning of a connection is valuable because
much of it is structured. A ClientHello has fields, extensions and ordering; it
may contain SNI and ALPN; it can be compared with known browser stacks; and even
after encryption starts, the record sizes and timing of the first few flights
often have recognizable patterns. This is why "TLS mimicry" is fragile: a tool
can use port 443 and still be detectable if it does not behave like the
implementation it imitates.

There is also a second trap: TLS inside TLS. If a proxy wraps arbitrary client
traffic, and the inner traffic often begins with another TLS handshake, the
outer flow can acquire a nested-handshake fingerprint. Research around
encapsulated TLS handshakes has shown that censors can sometimes detect
obfuscated proxies passively by the patterns created by nested protocol stacks.

The problem, then, is not only "encrypt the VPN", but the shape of the entire
flow.

## Current Families Of Evasion

Network steganography is not a new idea. One early network-specific example is
Manfred Wolf's "Covert Channels in LAN Protocols" from the LANSEC '89
proceedings, which treated LAN protocol behavior itself as a place where hidden
communication could exist. The modern censorship-circumvention ecosystem is
much larger and more practical, but it is still wrestling with the same basic
question: where can a communication channel hide without violating the
statistical and semantic expectations of its carrier?

Most current systems pick one of several strategies. Trojan tries to be a real
HTTPS server for unauthorized clients and a proxy for authorized clients, so
unauthenticated probes can be redirected to a fallback while authenticated
clients send a Trojan request inside TLS. Xray/VLESS/XTLS Vision and REALITY
focus on UUID-like identity, TLS-oriented transport behavior, padding and
fallback machinery. NaïveProxy takes a pragmatic route by using Chromium's
network stack so the client inherits browser-like behavior instead of
reimplementing it poorly. Cloak explores steganography and encryption around
HTTPS-looking traffic, zero-round-trip authentication, fallback behavior and
shaping, while Tor pluggable transports solve a different problem for a
different network but show the same lesson: protocol shape matters, not only
cryptographic strength. Balboa is an especially relevant academic neighbor
because it tunnels data through existing applications by rewriting traffic that
matches a shared traffic model, aiming to avoid distinguishable divergence from
the application's normal behavior.

These systems are serious work. FPS is not here to dismiss them. The point is
that the pressure has moved. If classifiers focus on startup behavior, handshake
details and active probes, then a useful next-generation design should avoid
introducing a custom protocol at startup at all.

## The FPS Thesis

FPS is a "+1 generation" approach because it shifts the authentication moment.
Instead of opening a connection that immediately tries to look like a browser,
FPS starts by relaying a real browser or application TLS session whose carrier
traffic can be HTTPS, WebSocket, long polling, a game client or another
long-lived TLS application flow.

Only after real carrier records already exist does FPS attempt a late
Zero-RTT upgrade inside a plausible TLS Application Data record. The upgrade is
bound to nearby carrier context, including the previous observed TLS record.

If the upgrade fails, the record is treated as ordinary cover traffic and the
relay continues. If it succeeds, both sides start classifying later TLS
Application Data records with transcript-bound hints and AEAD verification.
Ordinary carrier records remain visible and are forwarded byte-for-byte; FPS
records are separate TLS-like Application Data records whose opaque payloads
carry encrypted datagram/control frames and padding. The Linux VPN adapter maps
TUN packets to those opaque datagrams, but the wire layer is no longer tied to
IP packets.

![TLS record stream after FPS upgrade](assets/fps-tls-record-stream.svg)

The practical consequence is that a censor cannot only classify the first
handshake flight and move on. To catch FPS generically, it needs to keep
analyzing the L4 stream after a real TLS session is already underway, which is
more expensive and more collision-prone than checking connection startup. At
the optimistic end, with careful shaping, padding and carrier selection, FPS
tries to approach indistinguishability from ordinary TLS application traffic;
the current beta does not claim that yet, but the architecture is built so that
this is the direction of travel.

## What FPS Is

FPS has two Linux daemons:

- `fps_client`, running near the user;
- `fps_server`, running near the operator-controlled origin side.

Architecturally, FPS has a reusable covert datagram core and one primary Linux
adapter. The core registers authenticated carrier sessions, fragments and
reassembles best-effort opaque datagrams, and schedules them over one or more
carriers. The Linux adapter creates TUN interfaces on both sides; applications
send IP packets into the TUN device, and the adapter maps those packets to
opaque FPS datagrams.

On the wire between client and server, an observer should see TCP streams made
of TLS Application Data records. The real browser/origin TLS stream is not
terminated by FPS. Before upgrade, FPS is a transparent relay. After upgrade,
ordinary carrier TLS records continue to be forwarded byte-for-byte, while FPS
inserts separate classified TLS Application Data records for encrypted
datagram/control payloads.

![FPS architecture](assets/fps-architecture.svg)

The server owns address assignment. A client is represented by one secret UUID.
The server allowlists UUIDs, assigns an IPv4 lease after authentication and
drops client TUN packets whose IPv4 source does not match the assigned lease.
Multiple carrier sessions from the same client process can be used as a carrier
pool. If another active process uses the same UUID, the newer instance replaces
the older one.

The implementation is Linux/Docker first. Android is planned, but not built
yet. The core code is split so authentication, record classification, carrier
pooling and opaque datagram framing can be reused behind Android `VpnService`
or another future adapter instead of inheriting Linux TUN, IPv4 lease policy and
`ip` commands.

## Hosting: FPS As A Pre-Reverse-Proxy

In the clean self-hosted model, `fps_server` is the public gate in front of the
carrier origin path. It does not terminate the carrier TLS session. It behaves
more like a pre-reverse-proxy or TCP gate.

The chain can look like this:

```text
Internet -> fps_server -> L4/TCP proxy or SNI router -> real reverse proxy -> app
```

The real reverse proxy is where TLS terminates and the backend application
begins. FPS stays before that point so it can observe and preserve the carrier
TLS byte stream.

This is why ordinary CDN termination is a problem. If the CDN terminates TLS
and opens a separate connection to your origin, the end-to-end carrier byte
stream FPS relies on is gone; FPS can work behind products and deployments that
offer L4/TCP proxying, TLS passthrough or SNI routing, but those are less common
than mainstream HTTP reverse proxies and often priced or operated as more
specialized infrastructure. It is not "put it behind any CDN and forget about
it" software.

## Key Features

### Late Authentication

FPS does not need to advertise itself at byte zero. The current construction
uses X25519, HKDF-SHA256 and ChaCha20-Poly1305 through OpenSSL. The client UUID
derives an internal X25519 key pair. The server has a static key pair and an
allowlist of client UUIDs.

The project does not invent new cryptographic primitives. It uses existing
ones and tries to compose them carefully: transcript binding, short
server/client hints, AEAD associated data, metadata discipline and failure
semantics are the important parts.

There is still review work to do. The current v4 design removed the older
fixed-position public-key-shaped Zero-RTT prefix and replaced it with
transcript-bound opaque hints. The remaining question is whether this
no-timestamp/no-cache replay model is the right default for a controlled beta
or whether durable replay state should return after independent protocol
review.

### Carrier Pooling

Any authenticated carrier session can carry opaque FPS datagrams. FPS no longer
has a single "primary session" concept, so multiple browser tabs or application
flows can become part of the carrier pool. In the current VPN adapter, those
datagrams are TUN packets; in a future adapter, the same core could carry a
different unreliable payload without changing the carrier authentication layer.

### L3 TUN Instead Of Only SOCKS

The current FPS product adapter carries IP packets through Linux TUN. A SOCKS
proxy can be layered on top for convenience, and the documentation includes a
Dante overlay example, but the hidden transport underneath is a generic
best-effort datagram channel. TUN remains the first and most important adapter
because it turns FPS into a VPN framework, but it is not the protocol core.

### Room For Shaping

The beta already supports opaque datagram fragmentation and envelope padding
hooks. For the current TUN adapter, that means near-MTU IP packets can be split
before transmission. The future shaping layer can decide how many bytes to
inject into a carrier, how to split payloads, how to throttle hidden traffic and
how to spread load across carriers.

Without shaping, FPS is a structural camouflage experiment. With shaping, it
becomes a stronger traffic-analysis experiment.

### Protocol Family Potential

TLS is the first carrier because it is everywhere, but the underlying idea is
larger: ordinary encrypted application flows can become carrier media. The same
family of ideas could target SSH, WebRTC data channels, collaboration
protocols, QUIC-like application flows or other long-lived encrypted streams.

The model can also be stacked. In principle, one FPS channel can carry traffic
that later becomes another FPS carrier, as long as deployment order is planned
carefully, for example `C1 -> C2 -> S2 -> S1` for two nested client/server
pairs. This is not a polished user feature today, but it is a useful way to
think about FPS as a family of composable carrier protocols rather than one
fixed proxy binary.

## Costs And Limits

FPS requires live carrier sessions, so a leased TUN address only means the
client authenticated and received an address; if all carriers close, VPN traffic
cannot flow. FPS also needs a carrier path that preserves the TLS byte stream,
which means TLS termination by a CDN or ordinary reverse proxy changes the
model.

The UX is still beta. Operators deal with Docker, config files, UUIDs,
`/etc/hosts` mappings, TUN capabilities, status sockets and carrier liveness,
which is reasonable for controlled beta deployments but not consumer VPN UX.
There is no mobile client yet, and Android matters because many users
experience filtering on mobile networks first; it also has its own constraints,
including one active VPN service, loop prevention, route ownership and app-level
network APIs.

Finally, FPS is not anonymity software: it does not replace Tor, it does not
claim global unlinkability and it should be understood as a hidden
transport/tunnel framework.

## Comparison With Nearby Systems

### Trojan

Trojan runs inside TLS and uses fallback behavior for unauthenticated traffic,
which makes it simpler to operate and easier to reason about. FPS differs by
using late upgrade inside a live carrier byte stream and by inserting separate
classified TLS records after authentication while ordinary carrier TLS records
continue byte-for-byte, so Trojan is best seen as a practical proxy protocol
while FPS is a more experimental carrier steganography framework.

### Xray / VLESS / XTLS Vision

VLESS uses UUID-style identity, and Vision/REALITY-oriented configurations are
part of a mature anti-censorship proxy ecosystem. FPS borrowed the UUID UX
lesson because distributing one client UUID is easier than distributing raw key
files, but the scope is different: Xray is a broad proxy platform, while FPS is
narrower and lower-level, a covert datagram transport whose current Linux
adapter is a leased L3 TUN VPN that tries to stay TLS-record-shaped inside real
carrier sessions.

### NaïveProxy

NaïveProxy's strongest idea is to reuse Chromium's network stack, which is a
very pragmatic way to inherit browser-like behavior. FPS does not try to be
Chrome; it tries to borrow real TLS sessions from Chrome or other applications
and hide a tunnel inside them.

### Cloak

Cloak is conceptually close: zero-round-trip authentication, fallback behavior,
steganographic framing and traffic-shaping ideas all matter there. FPS differs
mainly in the late-upgrade carrier model. It does not want the first connection
flight to be the circumvention protocol.

### Balboa

Balboa is probably the closest research relative in spirit. It is a link
obfuscation framework that tunnels data through existing applications by
intercepting outgoing network traffic and rewriting matching traffic according
to a pre-shared model. When used with TLS, its goal is to make application
behavior equivalent to the application running without Balboa, modulo small
timing differences.

FPS differs in deployment and protocol shape. It is an endpoint covert
datagram framework whose current Linux adapter provides TUN interfaces, leased
client addresses, late Zero-RTT authentication and encrypted FPS records over
visible TLS Application Data records. It does not currently require a
pre-shared content model for the carrier application; it relays the real
carrier stream byte-for-byte and, after upgrade, inserts separate classified
records carrying hidden datagrams/control frames.

### Tor Pluggable Transports

Tor pluggable transports transform Tor traffic so it can pass through hostile
networks. FPS is not a Tor transport and does not provide Tor's anonymity
properties. But both projects share a central lesson: the shape of traffic is a
security property.

### Shadowsocks

Shadowsocks is a secure proxy family where modern AEAD modes make traffic look
like random bytes. FPS chooses a different camouflage target: not random bytes,
but TLS Application Data records in a live carrier context.

Random-looking can be good. In some networks, ordinary-looking may be better.

## Development Process

FPS was built with heavy AI assistance, which is not a claim that AI makes
systems work automatically. It is the opposite: AI makes it cheap to generate
code, which means it must be surrounded by structure:

- a current protocol specification;
- a work log that records decisions and failures;
- an `AGENTS.md` file that tells future agents how to work safely;
- unit and integration tests;
- Docker simulations;
- sanitizer and Valgrind runs;
- coverage checks;
- bounded libFuzzer smoke tests;
- CI across GCC, clang, Ubuntu and Alpine Docker images.

The work log is not bureaucracy; it is how the project survives hard resets,
context loss and handoff between agents. The tests are not decoration either:
they are what keeps a protocol experiment from becoming a pile of
plausible-looking network code.

## What Comes Next

The near-term plan is to make FPS boring enough to criticize properly.

Priorities:

- independent protocol review;
- repeated two-host soak tests for release candidates;
- cleaner public documentation and onboarding;
- signed images and a stricter release process;
- better operator status and diagnostics;
- independent review of the transcript-bound Zero-RTT hints;
- Android client design through `VpnService`;
- realistic shaping and traffic-analysis experiments;
- exploration of non-TLS carrier families such as SSH and WebRTC.

The protocol-family idea is at least as important as this implementation,
because FPS is only one design point in a larger space of hidden channels inside
ordinary encrypted application flows.

## Please Break It

If you are interested in networking, censorship resistance, steganography,
protocol design or AI-assisted systems work, do not treat FPS gently.

Run it. Mock it. Laugh at it. Capture the packets. Try to classify it. Review
the Zero-RTT construction. Attack the assumptions. Find the UX traps. Tell me
where the protocol is naive, where the implementation is too clever and where
the documentation is lying by omission.

That is how a project like this becomes useful.

## Closing Disclaimer

FPS was created as an experimental research and engineering project, not as a
tool for harming networks or people. Its purpose is to explore modern network
architecture, protocol camouflage, test-heavy AI-assisted development and the
practical limits of building hidden communication channels over common
infrastructure.

Use it only where you are allowed to operate it, do not abuse third-party
services as carrier origins, and if you deploy it, use your own infrastructure
or systems where you have permission.

And if the name made the project harder to find, or if it made someone spend a
perfectly good evening doing something more relaxing instead of studying network
protocols, then at least one part of the design worked.

## UPDATE: FPS Records Are Now Separate TLS Records

The implementation has moved to a cleaner wire model since the first version of
this draft. After the late authentication step, FPS no longer needs to wrap the
carrier byte stream and covert payload into one monolithic encrypted envelope
that replaces every observed carrier TLS record. Ordinary carrier TLS records
continue to be forwarded byte-for-byte, while FPS inserts its own separate,
classified TLS Application Data records between them.

That distinction matters for engineering reasons. A visible FPS record is still
a syntactically normal TLS Application Data record on the `fps_client <->
fps_server` link, but internally it carries an encrypted FPS bundle with opaque
datagrams, control messages and padding. The current Linux TUN adapter maps one
VPN packet to one opaque datagram before any internal fragmentation. The real
browser/origin TLS records stay in the same ordered TCP stream and are not
terminated by FPS.

This design is more convenient for shaping and throttling because the covert
channel now has explicit record-sized scheduling units. A future shaper can
decide when to insert FPS records, how large they should be, how much padding to
add and how aggressively to pace them against the observed carrier cadence. In
other words, correctness and confidentiality no longer require FPS to transform
every carrier record; the shaping layer can operate on additional TLS-shaped
records instead.

The tradeoff is clear: FPS is syntactically TLS-shaped, but it still has to be
statistically shaped. Separate TLS records make that next layer easier to build,
but they do not make it automatic.

## UPDATE: Packet-Capture Flow Experiment

I also ran a small pcap experiment to stop arguing from intuition alone. The
test used a local Docker/TUN topology with one `fps_client`, one `fps_server`,
the debug HTTPS/WSS carrier, and bidirectional UDP `iperf3` over the FPS TUN
lease. Zero-RTT upgrade was intentionally delayed so the capture contained both
a pre-upgrade window and a post-upgrade window.

The full note lives in the repository as
[`docs/pcap-flow-analysis.md`](../docs/pcap-flow-analysis.md). The short result:
Wireshark/libpcap still see parseable TLS records on the FPS TCP link, but the
packet-size and timing distributions visibly change after FPS starts carrying
TUN traffic.

![Visible FPS TCP-link packet size and timing overview](../docs/assets/pcap-flow/bidir-overview.png)

![Packet-size and inter-packet quantiles by direction](../docs/assets/pcap-flow/bidir-quantiles.png)

In that run, the bidirectional UDP probe completed at about 2 Mbit/s in each
direction without packet loss. Before upgrade, the capture had a small carrier
window with a median IP packet size around 119 bytes and a p95 inter-packet
interval around 33 ms. After upgrade, the visible FPS link carried about 8.8
Mbit/s of IP traffic, the median packet size moved to about 1321 bytes, and the
p95 inter-packet interval dropped to about 4.3 ms.

The conclusion is not that FPS is already indistinguishable from its carrier.
It is the opposite and more useful conclusion: the current beta preserves TLS
record syntax, but a traffic analyst can still see the extra load as a dense
near-MTU band after upgrade. The next serious traffic-analysis work should
therefore focus on classified-record scheduling, pacing and padding rather than
on TLS record validity alone.

## References

- Manfred Wolf, "Covert Channels in LAN Protocols", in Local Area Network
  Security, LANSEC '89:
  https://doi.org/10.1007/3-540-51754-5_33
- RFC 8446, The Transport Layer Security Protocol Version 1.3:
  https://www.rfc-editor.org/rfc/rfc8446.html
- RFC 6066, TLS Extensions and Server Name Indication:
  https://www.rfc-editor.org/rfc/rfc6066.html
- Detecting Probe-resistant Proxies, NDSS 2020:
  https://www.ndss-symposium.org/ndss-paper/detecting-probe-resistant-proxies/
- The TLS Inside TLS Problem, Censored Planet:
  https://censoredplanet.org/fingerprint-encapsulated-tls
- Project X VLESS and XTLS Vision documentation:
  https://xtls.github.io/en/config/inbounds/vless.html
- Trojan protocol documentation:
  https://trojan-gfw.github.io/trojan/protocol.html
- NaïveProxy README:
  https://github.com/klzgrad/NaïveProxy/blob/master/README.md
- Cloak steganography and encryption notes:
  https://github.com/cbeuw/Cloak/wiki/Steganography-and-encryption
- Balboa: Bobbing and Weaving around Network Censorship:
  https://arxiv.org/abs/2104.05871
- Tor pluggable transports documentation:
  https://support.torproject.org/little-t-tor/tor-pluggable-transports/
- Shadowsocks AEAD-2022 specification:
  https://shadowsocks.org/doc/sip022.html
