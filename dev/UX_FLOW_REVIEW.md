# FPS UX Flow Review Snapshot

Date: 2026-05-29

This note is the current internal UX snapshot. Historical command transcripts,
public test-origin names and step-by-step experiment logs belong in
`dev/WORKLOG.md`, not here.

## Current Operator Flow

- Build or pull the FPS Docker image.
- Generate a server key pair with `fps_server --generate-server-keypair`.
- Generate one client UUID per device/profile with
  `fps_client --generate-client-uuid`.
- Configure `fps_server` with inline base64 server keys,
  `allowed_client_uuids`, carrier origin endpoint, TUN lease pool and optional
  `ops.status_socket`.
- Start `fps_server` and a carrier origin or point `fps_server` at a real
  operator-controlled carrier origin.
- Generate a client profile or `fps://v1` URI with
  `fps_server --generate-client-profile`.
- Import the profile on the client with `fps_client --write-config-from-uri` or
  mount the generated JSON directly.
- Start `fps_client` in host-network Docker mode or natively.
- Create at least one long-lived browser/application carrier session. For
  hostname-based origins, use `/etc/hosts` or router DNS to map the carrier
  hostname to the local `fps_client` listener while preserving SNI and `Host`.
- Confirm readiness with `--status`: a TUN lease alone is not enough;
  `sessions.carriers_current` must be positive.

## What Is Acceptable For Beta

- Docker-first setup is usable by a technical operator.
- Generated client profiles and `fps://v1` URIs avoid hand-written client key
  material.
- `/etc/hosts` carrier hostname mapping is simple and explicit. FPS should not
  ship a transparent DNS proxy in the current beta.
- `fps_carrier origin/client` is useful as a deterministic debug carrier and
  integration-test origin.
- Real-origin carrier flows are documented without recommending third-party
  public echo services as dependencies.
- The Dante overlay example is useful for applications that can use SOCKS5, but
  proxy daemons remain outside the base FPS image.
- Router/LAN-gateway client mode is a plausible deployment pattern, documented
  as experimental until it is validated on real router hardware or a close lab
  target.

## Remaining UX Friction

- Server-side first setup still has several manual values: server keys, client
  UUID, allowlist, origin endpoint, listen endpoint, lease pool and profile
  generation.
- There is no `doctor` command that checks common deployment mistakes such as
  missing live carriers, wrong hosts mapping, closed status socket, bad TUN
  capabilities or unusable proxy overlay.
- The product has no single deployment bundle generator. This is intentionally
  deferred until beta feedback confirms the stable file layout and operator
  workflow.
- Real-origin carrier guidance still expects the operator to understand SNI,
  `Host`, `/etc/hosts`, long-lived TLS sessions and the difference between
  lease readiness and carrier readiness.
- Router/LAN-gateway mode needs concrete OpenWrt/router validation before it can
  move from "experimental pattern" to supported flow.

## Near-Term UX Priorities

1. Keep documentation and examples short, current and Docker-first.
2. Add a narrow `doctor`/preflight command only after more beta runs show which
   checks save the most time.
3. Keep profile generation in FPS binaries rather than growing broad helper
   scripts that duplicate config validation.
4. Repeat manual user-flow tests before release candidates and update this
   snapshot only with current findings and unresolved friction.
