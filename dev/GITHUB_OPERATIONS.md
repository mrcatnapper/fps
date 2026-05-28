# GitHub Operations

This note covers the current public GitHub repository operations. It does not
replace the public release checklist in `docs/release.md`.

Current baseline:

- public remote: `git@github.com:mrcatnapper/fps.git`;
- default branch: `main`;
- working branch: `develop`;
- GitHub Pages source: `main:/docs`;
- image publishing: manual `Publish Images` workflow only.

## Routine Checks

Before opening or merging a pull request:

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

For release-candidate branches or risky protocol/runtime changes, also run:

```sh
tools/run_quality_checks.sh --all
FPS_DOCKERFILE=Dockerfile.alpine FPS_DOCKER_IMAGE=fps:alpine \
  tools/run_quality_checks.sh --docker
```

Root/TUN, pcap and long two-host soak checks remain local/manual release
candidate gates until a safe privileged runner exists.

## Artifact And Secret Scan

The repository must not track real configs, UUIDs, private keys, lease files,
pcaps, status sockets, coverage profiles or runtime state. Example configs under
`examples/`, deterministic fixtures under `tests/`, and public docs are expected
to contain placeholder field names.

Use these checks and review every match:

```sh
git status --short
git ls-files | rg -n '(\.pcap|\.pcapng|\.cap|\.profraw|\.profdata|\.status|\.sock|leases\.json|captures/)'
rg -n 'server_private_key_base64|client_uuid|fps://v1|BEGIN.*PRIVATE KEY|PRIVATE KEY-----' \
  --glob '!docs/**' --glob '!dev/**' --glob '!examples/**' --glob '!tests/**' \
  --glob '!tools/**' --glob '!src/**' --glob '!include/**'
```

The first `git ls-files` command should normally print nothing. The second
command should normally print nothing.

## Current Branch Protection

Current `main` branch protection:

- branch is protected;
- required status checks are strict, so branches must be up to date before
  merge;
- required checks:
  - `linux-local / gcc`;
  - `linux-local / clang`;
  - `docker-smoke / ubuntu-gcc`;
  - `docker-smoke / ubuntu-clang`;
  - `docker-smoke / alpine-gcc`;
- pull request review protection is enabled with zero required approvals while
  there are no write-access collaborators;
- admin enforcement is disabled, so repository admins keep an emergency bypass;
- force-pushes and branch deletion are disabled;
- linear history and conversation resolution are required.

Repository merge policy:

- squash merge: enabled;
- rebase merge: disabled;
- merge commits: disabled;
- update branch button: enabled;
- delete branch on merge: enabled for short-lived PR branches.

Keep `Quality` manual/scheduled rather than required for every PR until runtime
is stable enough to absorb its cost.

Security settings currently enabled:

- Dependabot vulnerability alerts;
- Dependabot automated security fixes.
- secret scanning;
- secret scanning push protection.

## CI Budget And Push Discipline

Prefer local commits and local verification while iterating. Push to GitHub when
there is a useful review, handoff, release-candidate or integration checkpoint;
do not use every small local edit as a remote CI trigger.

For documentation-only or repository-hygiene commits that do not touch source
code, tests, build files, Docker files or workflows, GitHub Actions supports
skip instructions in commit messages for `push` and `pull_request` workflows:

```text
[skip ci]
[ci skip]
[no ci]
[skip actions]
[actions skip]
```

Alternatively, a `skip-checks: true` trailer can be used. Official reference:
<https://docs.github.com/en/actions/how-tos/manage-workflow-runs/skip-workflow-runs>.

Do not skip CI for code, protocol, config, Docker, test or workflow changes.
Also avoid skip markers when required branch checks are enabled: GitHub can
leave skipped required checks in `Pending`, which blocks merge until a new
non-skipped commit is pushed.

On `develop` and short-lived feature branches, squash/amend and
`git push --force-with-lease` are acceptable for typo fixes, large local squash
cleanup or replacing noisy intermediate commits. Do not force-push `main`, and
do not rewrite commits that another contributor or agent may have based work on
without coordination.

## Image Publication

The repository includes one manual `Publish Images` workflow. It performs the
Docker runtime smoke first. With `publish=false`, it is a release-candidate dry
run and does not log in to GHCR or publish images. With `publish=true`, it
pushes Ubuntu and Alpine runtime images to GitHub Container Registry.

Current image policy:

- image registry: GitHub Container Registry under the repository owner;
- tags: one Ubuntu runtime tag and one explicit Alpine runtime tag, for example
  `ghcr.io/<owner>/fps:v0.1.0-beta.1`,
  `ghcr.io/<owner>/fps:v0.1.0-beta.1-alpine`, or when no version is supplied,
  `ghcr.io/<owner>/fps:<short-sha>` and
  `ghcr.io/<owner>/fps:<short-sha>-alpine`;
- images: Ubuntu runtime and Alpine runtime in the same package;
- publishing trigger: manual only;
- no `latest`, public GitHub Release or signed artifacts until release policy is
  approved.

The workflow explicitly sets `provenance: false` and `sbom: false` on
`docker/build-push-action`. This avoids extra untagged OCI package versions in
GHCR. Re-enable provenance/SBOM only as part of a separate
signing/attestation policy review.

Required workflow permissions when publication is enabled:

```yaml
permissions:
  contents: read
  packages: write
```

For future signed images or provenance, plan separate review for:

```yaml
permissions:
  contents: read
  packages: write
  id-token: write
  attestations: write
```

Do not enable those broader permissions until the signing/provenance tooling is
implemented and reviewed.

Manual local GHCR push remains available for experiments:

```sh
echo "$GHCR_TOKEN" | docker login ghcr.io -u "$GITHUB_USER" --password-stdin
docker build -t ghcr.io/OWNER/fps:v0.1.0-beta.1 .
docker build -f Dockerfile.alpine -t ghcr.io/OWNER/fps:v0.1.0-beta.1-alpine .
docker push ghcr.io/OWNER/fps:v0.1.0-beta.1
docker push ghcr.io/OWNER/fps:v0.1.0-beta.1-alpine
```

For manual pushes, use a fine-grained token or classic PAT with package write
scope only for the duration of the experiment. Prefer the workflow path for
repeatable release candidates because it records the exact commit, smoke result
and pushed tags.

## GitHub API Access For Agents

This workspace currently has authenticated `gh` CLI access with repository admin
permissions. If a future workspace lacks GitHub access, provide one of:

- installed and authenticated `gh` CLI with repository access; or
- `GH_TOKEN`/`GITHUB_TOKEN` in the environment.

Minimum useful scopes for a fine-grained repository token:

- repository contents: read;
- actions/workflows: read, and write only if the agent should dispatch
  workflows;
- packages: read for image inspection, write only for publication workflows.

Keep publication tokens out of committed files and do not grant write scopes for
ordinary code-review/check-only work.

## Protocol Review Packet

Send reviewers:

- `docs/specification.md`;
- `docs/testing.md`;
- `docs/beta-status.md`;
- `dev/PROTOCOL_REVIEW_BRIEF.md`;
- relevant test names from `ctest -N`, especially Zero-RTT, envelope,
  adversarial and TUN lease-routing tests.

Ask reviewers to focus on Zero-RTT transcript binding, hint precheck,
no-timestamp/no-cache replay assumptions, envelope failure semantics and
leased-client source enforcement.
