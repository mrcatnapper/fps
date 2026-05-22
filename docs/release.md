# Release Checklist

This checklist is for beta release candidates. It does not publish artifacts by
itself.

The project license is MIT; keep `LICENSE` in every source distribution.

This checklist remains focused on beta release candidates. Image publication,
signed artifacts and GitHub Release uploads are deliberate manual steps until
release policy is finalized.

## Preflight

```sh
git status --short
python3 -m py_compile tests/integration/*.py tools/*.py
bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh
cmake --build build -j 2
ctest --test-dir build --output-on-failure
ctest --test-dir build -L local --output-on-failure
python3 tests/integration/docker_artifacts.py --repo /workspaces
git diff --check
```

Confirm no real configs, UUIDs, private keys, lease files, captures, build
directories or local runtime artifacts are tracked.

```sh
git status --short
rg -n 'server_private_key_base64|client_uuid|fps://v1|BEGIN.*PRIVATE KEY' \
  --glob '!docs/**' --glob '!examples/**' --glob '!tests/**'
```

Review any match manually; examples and docs should contain placeholders only.

## CI And Quality

After pushing to GitHub:

1. Run the `CI` workflow for GCC, clang, Ubuntu Docker and Alpine Docker smoke.
2. Run the scheduled/manual `Quality` workflow once for ASan/UBSan, Valgrind,
   llvm-cov and bounded fuzz smoke.
3. Run the opt-in Docker smoke locally when CI Docker permissions differ from
   the release host:

   ```sh
   FPS_DOCKER_COMPILER=gcc FPS_DOCKER_IMAGE=fps:local \
     tools/run_quality_checks.sh --docker
   ```

## Docker Images

Build both runtime variants:

```sh
docker build -t fps:local .
docker build -f Dockerfile.alpine -t fps:alpine .
```

The repository has one manual image workflow:

- `Publish Images` performs the Docker runtime smoke and can push Ubuntu and
  Alpine images to GHCR when `publish=true`.
- Leave `publish=false` for a release-candidate dry run. This validates the
  same build/smoke path without logging in to GHCR or publishing images.

Recommended GHCR tags for a pre-release:

```text
ghcr.io/OWNER/fps:v0.1.0-beta.1
ghcr.io/OWNER/fps:v0.1.0-beta.1-alpine
```

The unsuffixed version tag is the default Ubuntu runtime image. Do not publish
or document `latest` until release policy is explicit. The publish workflow
intentionally disables Buildx provenance/SBOM attestations so GHCR shows only
the two runtime image tags above; signed images and provenance remain explicit
future release-hardening work.

Run at least one Docker/TUN simulation:

```sh
FPS_DOCKER_SUDO=1 tools/docker_tun_iperf_sim.py --image fps:local \
  --duration 10 --bandwidth 250K --length 512
```

Repeat the two-host soak described in [testing.md](./testing.md) for release
candidates. Public release should not rely on a single historical pass.

## Version And Tag

Use pre-release tags until compatibility policy is explicit:

```text
v0.1.0-beta.1
```

Release notes should include:

- supported platforms: Linux Docker, Ubuntu and Alpine images;
- current config/protocol version;
- known limitations: no Android app, no advanced shaper, no config hot reload,
  no shared UUIDs, no public protocol audit yet;
- upgrade notes for config/schema changes since the previous tag.

## Rollback

Keep the previous Docker image tag available. A rollback means:

1. stop current containers;
2. restore the previous image tag and config;
3. keep or restore `/var/lib/fps/leases.json` only if the same client UUID set is
   still valid;
4. restart server, then clients, then carrier/proxy overlays;
5. verify `--status` and a TUN probe.

## Remaining Release Hardening

- Signed Docker images.
- Privileged root/TUN CI runner.
- Independent Zero-RTT/protocol review.
- Repeated release-candidate two-host soak automation.
