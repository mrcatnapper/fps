# UUID And Key Rotation

FPS beta identity is intentionally simple:

- one secret `client_uuid` per device/profile;
- one server X25519 key pair in server config;
- server-owned IPv4 leases keyed by the derived client public key.

There is no config hot reload in this increment. Apply allowlist or key changes
by updating config, validating it, then restarting/redeploying the affected FPS
daemon.

## Reissue One Client UUID

Use this when a device is replaced, a profile is suspected to have leaked, or an
operator wants to move a user to a fresh UUID.

1. Generate a new UUID:

   ```sh
   NEW_UUID="$(fps_client --generate-client-uuid)"
   ```

2. Add the new UUID to `security.zero_rtt.allowed_client_uuids` in the server
   config.

3. Generate and deliver a new client profile or URI:

   ```sh
   fps_server --generate-client-profile \
     --config server.json \
     --client-uuid "$NEW_UUID" \
     --server-endpoint fps.example.net:443 \
     --format uri
   ```

4. Remove the old UUID from `allowed_client_uuids`.

5. Revoke the old lease and prune stale leases:

   ```sh
   fps_server --lease-revoke-client-uuid "$OLD_UUID" --config server.json
   fps_server --lease-prune --config server.json
   ```

6. Validate and restart the server:

   ```sh
   fps_server --check-config --config server.json
   ```

7. Verify with:

   ```sh
   fps_server --lease-list --config server.json
   fps_server --status --config server.json
   ```

## Revoke A Lost Client

1. Remove the UUID from `allowed_client_uuids`.
2. Revoke its lease:

   ```sh
   fps_server --lease-revoke-client-uuid "$LOST_UUID" --config server.json
   ```

3. Run `fps_server --lease-prune --config server.json`.
4. Restart the server so the new allowlist is active.
5. Confirm `--lease-list` no longer shows the revoked lease fingerprint.

The lease file stores public-key fingerprints and IP metadata, not UUID strings
or private material.

## Rotate The Server Key Pair

Server key rotation changes `server_public_key_base64`, so every client profile
must be regenerated. Treat it as a planned outage.

1. Generate a new server key pair:

   ```sh
   fps_server --generate-server-keypair
   ```

2. Replace `server_private_key_base64` and `server_public_key_base64` in the
   server config.

3. Regenerate every client JSON profile or `fps://v1` URI. Existing client
   profiles cannot authenticate against the new server public key.

4. Validate and restart the server:

   ```sh
   fps_server --check-config --config server.json
   ```

5. Redeploy client configs and confirm new carrier authentication through
   `--status`.

Existing lease assignments are derived from client identity, not the server key,
but a conservative rotation can still prune leases and let clients reacquire
addresses if the operator wants a clean state.

## Validation Drill

The current beta candidate has been exercised with a local Docker rotation
drill:

- an old generated client profile authenticated and passed a short UDP TUN
  probe;
- after allowlist update, `--lease-revoke-client-uuid` and `--lease-prune`, the
  stale old profile no longer authenticated;
- a regenerated profile for the new UUID authenticated and passed the same TUN
  probe;
- after server key-pair rotation, the profile containing the old
  `server_public_key_base64` no longer authenticated;
- a regenerated profile containing the new server public key authenticated and
  passed the TUN probe.

Repeat this drill for release candidates or after changing profile, lease or
server-key tooling. When generating profile files from a root-running Docker
container into a bind mount, use host redirection, `--user "$(id -u):$(id -g)"`
or an explicit ownership fix so the host operator can read the `0600` output.

## Unsupported Patterns

- Do not share one UUID across devices. The duplicate policy is `replace_old`,
  where the newer active instance supersedes older carriers for the same UUID.
- Do not treat `fps://v1` URIs as public links; they contain the client UUID
  bearer secret.
- Do not expect `allowed_client_uuids` changes to affect a running daemon until
  it is restarted or redeployed.
