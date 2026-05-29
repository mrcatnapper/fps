# FPS Postman Origin Deployment UX Flow Review

Date: 2026-05-17

Status: historical internal UX report. Public/operator documentation has since
been rewritten to avoid recommending third-party public test services as carrier
origins. Keep the concrete origin names here only as evidence from that manual
test run.

## Summary

I reran the full two-host deployment flow as a beta-style operator with average
DevOps skills:

- public FPS server on `fpshop`, public port `443`;
- local FPS client on `127.0.0.1:443`;
- real public carrier origin `wss://ws.postman-echo.com/raw`;
- no `fps_carrier.py` usage for origin or client-side carrier generation;
- manual carrier sessions opened with a normal Python WebSocket client;
- application traffic tested through the server-side Dante SOCKS5 overlay;
- runtime containers, volumes and runtime folders cleaned up afterward.

The result was positive. The tunnel authenticated real manual carrier sessions,
assigned a TUN lease, kept three concurrent Postman WebSocket carriers alive, and
carried SOCKS HTTPS traffic with the VM public IP visible externally. No
auth/envelope/codec/TLS parse failure appeared in the reviewed logs.

## Origin Choice

Chosen origin: `wss://ws.postman-echo.com/raw`.

Reasoning:

- It is a real public WebSocket echo endpoint documented by Postman.
- It supports ordinary TLS SNI/Host `ws.postman-echo.com` and path `/raw`.
- It can be driven by a generic WebSocket client or GUI tool, not by FPS tooling.
- Direct preflight succeeded: a message sent to
  `wss://ws.postman-echo.com/raw` was echoed back.

## Flow Performed

Build and transfer:

- Built `fps:local` from the current repo.
- Built the official Dante overlay as `fps-dante-proxy:local`.
- Loaded both images onto `fpshop` with `docker save ... | ssh fpshop docker load`
  to avoid building on the small VM.
- No packages were installed on the host or VM during this run.

Runtime state was created outside the repo:

- local client runtime: `/tmp/fps-postman-flow/client`;
- remote server runtime: `/root/fps-postman-flow/server`;
- manual carrier/test scratch path: `/tmp/fps-postman-flow/manual`.

Fresh runtime secrets were generated:

- one server X25519 keypair with `fps_server --generate-server-keypair`;
- one client UUID with `fps_client --generate-client-uuid`.

Server config:

- `network.listen`: `0.0.0.0:8443`, published as host `443`;
- `network.origin`: `ws.postman-echo.com:443`;
- `security.zero_rtt.profile_id`: `postman-echo-ws-v1`;
- TUN `fpss0`, server address `10.66.0.1/30`, lease pool `10.66.0.0/30`;
- status socket `/run/fps/server.status`;
- Dante SOCKS5 overlay listening on `10.66.0.1:1080`.

Client config:

- generated with `fps_server --generate-client-profile`;
- `network.listen`: `127.0.0.1:443`;
- `network.server`: `REDACTED:443`;
- TUN `fpsc0`, `auto_configure=true`;
- status socket `/run/fps/client.status`.

Validation:

- `fps_server --check-config` passed locally and on `fpshop`;
- `fps_client --check-config` passed locally;
- server and SOCKS overlay started with `docker compose --profile socks up -d`;
- local client started with host-network compose;
- no DNS, firewall, NAT, default-route or system resolver changes were made.

## User Scenarios

### 1. Direct Origin Preflight

I connected directly to `wss://ws.postman-echo.com/raw` with Python
`websockets`, sent one text message and received the same message back. This
confirmed the public origin was alive before involving FPS.

### 2. One-Shot Manual Carrier

I opened a WebSocket to the local FPS client while preserving the real origin:

- TCP endpoint: `127.0.0.1:443`;
- TLS SNI / WebSocket URL / Host: `ws.postman-echo.com`;
- path: `/raw`.

Result:

- Postman echo worked through FPS.
- FPS authenticated the carrier and assigned `10.66.0.2`.
- The session closed after the one-shot WebSocket finished.
- With `carriers_current=0`, TUN ping failed. This is correct behavior, but it is
  a user-facing trap: having a lease is not enough; at least one carrier must be
  currently open.

### 3. Persistent Manual Carriers

I then ran a small generic WebSocket client, not `fps_carrier.py`, holding three
concurrent WebSocket connections through `127.0.0.1:443` to the Postman origin.
Each connection sent one echo message per second.

Result:

- all three sessions authenticated;
- status showed `carriers_current=3`;
- echo RTT was typically around `94-101ms`;
- the connections stayed healthy for the duration of the SOCKS/TUN tests.

### 4. TUN Reachability

With persistent carriers open:

- `ping -c 3 10.66.0.1` succeeded;
- observed RTT was about `6.4-6.8ms`;
- TUN packet counters increased on both sides.

### 5. SOCKS Browsing / Fetching

Using `curl --socks5-hostname 10.66.0.1:1080`:

- `https://api.ipify.org?format=json` returned `REDACTED`, the VM public
  IP;
- `https://www.example.com/` returned HTTP 200;
- `https://postman-echo.com/get?fps=socks-flow` returned the expected JSON;
- a POST to `https://postman-echo.com/post` completed through SOCKS;
- Dante logs showed accepted SOCKS connections from `10.66.0.2` and outbound
  TCP connects to public HTTPS destinations.

### 6. Browser-Like HTTPS Carrier Attempt

I also tried a plain HTTPS request with `curl --resolve
ws.postman-echo.com:443:127.0.0.1 https://ws.postman-echo.com/raw`. This
preserves SNI/Host like a hosts-file override would, but it is not a WebSocket
upgrade. The origin returned HTTP 404 and the carrier closed quickly.

This is useful as a UX finding: a browser or ordinary HTTPS fetch can create a
short authenticated carrier, but for this origin it does not keep the tunnel
usable. Operators need a persistent browser tab/app/WebSocket client, not just a
single page load.

## Final Status Snapshot Before Cleanup

Client:

- `sessions.accepted=8`, `active=3`, `closed=5`;
- `carriers_current=3`, `carriers_registered=8`, `carriers_removed=5`;
- `auth.authenticated=8`;
- `auth.precheck_failed=0`, `unknown_client=0`, `decrypt_failed=0`,
  `server_accept_failed=0`;
- `envelope.decode_failed=0`, `encode_failed=0`, `tampered_or_invalid=0`;
- `tun.leased_client_address=10.66.0.2`;
- TUN counters: `packets_from_device=746`, `bytes_from_device=191879`,
  `packets_to_device=949`, `bytes_to_device=929758`;
- no `write_queue_full`, `codec_error` or `tls_record_error`.

Server:

- `sessions.accepted=8`, `active=3`, `closed=5`;
- `carriers_current=3`, `carriers_registered=8`, `carriers_removed=5`;
- `auth.authenticated=8`;
- `envelope.decode_failed=0`, `encode_failed=0`, `tampered_or_invalid=0`;
- `tun.leases_assigned_total=8`, `leases_persisted=1`;
- TUN counters: `packets_from_device=956`, `bytes_from_device=930094`,
  `packets_to_device=736`, `bytes_to_device=191291`;
- no `write_queue_full`, `codec_error` or `tls_record_error`.

Log review:

- no `envelope_encode_error`;
- no `event=envelope_error`;
- no `tls_parse_error`;
- no `codec_error`;
- no `tls_record_error`;
- no `reason=tcp_error` / `error=read_error`;
- closed authenticated one-shot sessions were reported as `peer_eof`.

## What Worked Well

- The current status JSON is much better for operator diagnosis. The separate
  `auth` and `envelope` groups make it clear that authentication succeeded and
  envelope mode remained healthy.
- `sessions.recent_closed` made short carrier closures understandable; they were
  `peer_eof`, not transport failures.
- Generated client profiles worked cleanly. I did not need to hand-edit client
  JSON.
- The Dante overlay worked as a practical application-level proxy once carriers
  were open.
- Cleanup with compose was straightforward: `down -v --remove-orphans` removed
  containers and volumes, and the TUN devices disappeared after daemon stop.
- Postman Echo is a workable public real-origin smoke target for manual
  WebSocket carrier testing.

## Friction And Issues

- The recommended public beta quickstart is now centered on a self-hosted debug
  carrier and `fps_carrier`. That is good for deterministic testing, but it does
  not teach a real-origin flow without FPS-specific carrier tooling.
- Server setup is still too manual: generate keypair, generate UUID, assemble
  server JSON, write compose, transfer images/configs, then generate the client
  profile. An average DevOps user can do it, but it is easy to paste one value in
  the wrong place.
- The weak-VM path is not documented as a first-class flow. Building locally and
  running `docker save | ssh docker load` is the right move here, but I had to
  infer it.
- "Lease assigned" can be mistaken for "VPN ready". In practice, the tunnel is
  only usable while `sessions.carriers_current > 0`.
- Manual carrier lifecycle is the biggest product UX gap. A user must understand
  that a normal WebSocket client/browser tab is now part of tunnel liveness. If
  that process exits, SOCKS/VPN traffic stops.
- A plain HTTPS page load is not necessarily a useful carrier. For Postman, a
  non-WebSocket GET to `/raw` returned 404 and closed quickly.
- At the time of the run, docs did not provide enough copy/paste examples for
  generic real-origin carriers: browser with hosts override, `curl --resolve`
  for short HTTPS checks, Python `websockets`, `websocat`, or other common
  tools.
- `tun.leases_assigned_total` increases per authenticated carrier even though
  `leases_persisted=1`. That is technically explainable, but the name can read
  like multiple leased IPs were assigned.
- SOCKS overlay defaults are convenient but insecure for broader deployments:
  no SOCKS auth, broad `10.66.0.0/24` allowlist, and no explicit warning in the
  quick path.
- Using a public echo service is operationally fragile. It can rate limit, change
  behavior or disappear. It is suitable for UX testing, not a recommended
  production carrier strategy.

## Follow-Up Status

Docs-only follow-up was completed after this review:

- document the real-origin carrier flow without `fps_carrier`;
- document router/LAN-gateway DNS override as a plausible but unvalidated
  deployment pattern;
- make the lease-vs-live-carrier readiness distinction explicit;
- add weak-VM image transfer, SOCKS smoke and cleanup commands to public docs;
- remove public-provider-specific carrier recommendations from user docs.

Still deferred:

- Add `fps_deploy init` or an equivalent bundle generator:
  - inputs: origin host/port, public server endpoint, local listen address,
    lease pool, enable SOCKS yes/no;
  - outputs: server config, client config/URI, server compose, client compose,
    exact validation and smoke commands;
  - writes secret files with safe permissions and avoids root-owned bind-mount
    surprises.
- Add a real-origin manual carrier guide with at least:
  - browser plus `/etc/hosts` flow;
  - `curl --resolve` for short HTTPS carrier sanity checks;
  - generic browser/application or operator-controlled WSS carrier examples;
  - clear explanation that carrier liveness is required for TUN/SOCKS liveness.
- Add a `doctor` status command that prints readiness in human terms:
  - config valid;
  - daemon running;
  - TUN lease received;
  - current carriers count;
  - SOCKS reachable;
  - last close reason;
  - "lease exists but no current carrier" as a specific warning.
- Add a documented weak-VM deployment path:
  `docker build` locally, `docker save | ssh docker load`, then remote compose
  up.
- Add official SOCKS smoke commands:
  - public IP check through SOCKS;
  - simple HTTPS GET through SOCKS;
  - optional POST/upload check;
  - expected Dante log signatures.
- Add cleanup commands for the two-host flow:
  local compose down, remote compose down, remove runtime directories, remove
  hosts entry if one was added, verify port `443` and TUN devices disappeared.
- Rename or supplement `leases_assigned_total` with a clearer current lease
  field so users do not confuse carrier assignments with distinct clients/IPs.

## Cleanup Performed

After testing:

- stopped the manual WebSocket carrier process;
- ran local `docker compose down -v --remove-orphans`;
- ran remote `docker compose --profile socks down -v --remove-orphans`;
- removed `/tmp/fps-postman-flow`;
- removed `/root/fps-postman-flow` on `fpshop`;
- verified no `fps-postman*` containers remained;
- verified local and remote port `443` were no longer listening;
- verified `fpsc0` and `fpss0` were gone.

Docker images/build cache were left in place intentionally. They are not running
services and are useful for subsequent developer checks.
