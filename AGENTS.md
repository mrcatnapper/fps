# AGENTS.md

Instructions for Codex/agents working on the FPS project in this workspace.

Scope: the whole repository from the directory that contains this file.

## Project Context

- The current main specification is `./docs/specification.md`.
- User/operator documentation lives in `./docs/`.
- Agent and developer working artifacts live in `./dev/`
  (`WORKLOG.md`, `ROADMAP.md`, `REVIEW.md`, `NETWORK_RECOVERY.md` and similar
  files). The repository root should keep only this `AGENTS.md` and the short
  `README.md`.
- FPS / Free Porn Storage is an experimental hidden L3 TUN tunnel carried over a
  live cover TLS session.
- Before major changes, reread the specification and the current work log.

## Version Control

- Use Git as the source of truth for change history.
- If the Git repository has not been initialized yet, initialize it at the
  workspace root before starting serious implementation work.
- Commit changes regularly in small logical chunks.
- Commit messages must be meaningful: state what changed and why.
- Prefer local commits and local verification during normal development. Do not
  push every small edit just to trigger GitHub CI; use remote CI for review,
  release-candidate and integration checkpoints.
- If a push is necessary for documentation-only or repository-hygiene changes
  that do not affect source code, tests, build files, Docker files or workflows,
  use a GitHub Actions skip marker such as `[skip ci]` in the commit message
  when branch policy allows it. Do not skip CI for source/runtime/config changes.
- On `develop` and short-lived feature branches, squash, amend and
  `--force-with-lease` pushes are allowed when they make review history clearer
  and do not overwrite another person's work. Never force-push `main`.
- To recover details, use `git log`, `git show`, `git diff`, `git blame` and the
  work log, not only the model's current context.
- Check `git status --short` before starting work.
- Do not revert changes made by the user or other agents unless explicitly
  asked.

## Communication With The User

- If an architectural decision substantially affects security, detectability,
  portability, testability or schedule, start a discussion with the user.
- Ask questions when context is incomplete and a reasonable assumption could
  cause expensive rework.
- You may and should challenge user proposals when you see false premises,
  missing risks or a simpler path.
- Keep discussion technical and concrete: explain tradeoffs, consequences and
  the recommended default.

## Planning And Work Log

- Before a noticeable task, provide a short plan: what will change, which files
  are involved and how the result will be verified.
- Keep the Markdown work log in `dev/WORKLOG.md`.
- In the log, record date, goal, decisions, completed steps, verification
  commands, open questions and links to relevant commits.
- After context reset or handoff to another agent, first read `AGENTS.md`,
  `dev/WORKLOG.md`, `docs/specification.md` and Git history.
- For both incremental and strategic planning, prefer the Pareto principle
  (80/20): prioritize useful changes that close confirmed functionality, UX and
  QoL issues instead of implementing features only because they sound generally
  nice or marketable.

## Development Practices

- Prefer simple, testable architecture over complex generalization.
- Follow existing project patterns. If patterns do not exist yet, introduce the
  smallest clear interfaces implied by the specification: `TunRuntime`,
  `TlsRecordParser`, `ZeroRttUpgradeEngine`, `EnvelopeCodec`, `CovertCodec`,
  `Shaper`, `SessionManager`/carrier pool.
- Apply the C++ Core Guidelines where they improve safety and clarity without
  bloating the code.
- Do not add abstractions without a concrete reason: testability, platform
  isolation, an explicit contract or meaningful duplication reduction.
- Handle errors explicitly. Network, file, crypto/config and TUN errors must
  have diagnosable paths.
- Do not log secrets, UUIDs, keys, raw payloads, nonces, session keys or raw
  upgrade material.

## C++ Baseline

- Language: C++20.
- Default compiler: `g++`.
- Preferred async style: Boost.Asio with C++20 coroutines where it simplifies
  control flow.
- Use Boost broadly when it reduces external dependencies:
  - Asio/networking;
  - Boost.Test;
  - Boost.JSON or Boost.PropertyTree for JSON/config if no third-party parser
    has been agreed;
  - Boost.Log when logging is needed;
  - Boost.Container/UUID/ProgramOptions where appropriate;
  - Boost.Asio SSL/OpenSSL integration for TLS-related tooling when useful.
- Do not add heavy external dependencies for minor convenience.
- RAII is required for file descriptors, sockets, TUN handles, timers,
  temporary files and crypto contexts.
- Do not block the event loop with long synchronous operations.
- For byte buffers, use explicit types (`std::byte`, `std::uint8_t`) and check
  bounds.

## Dependencies And Package Installation

- First check what is already installed in the container.
- If an external dependency is needed, explain to the user:
  - why it is needed;
  - why Boost/stdlib is insufficient;
  - whether it is a runtime or build/test dependency;
  - how to install or build it.
- After discussion, you may use `apt` or build a dependency from source.
- Do not add system packages just in case.
- Record installed dependencies in `dev/WORKLOG.md` and the build
  documentation.

## Testing

- Any meaningful code change should come with unit or integration tests.
- For C++ unit tests, use Boost.Test unless the user agrees otherwise.
- Cover edge cases:
  - fragmented/coalesced TLS records;
  - EOF and TCP half-close;
  - backpressure and queue overflow;
  - invalid Zero-RTT candidate, replay and tamper;
  - MTU/MSS and oversized TUN packets;
  - deterministic shaper seed;
  - empty/broken config files;
  - Android VPN loop-prevention abstraction when touched.
- Before committing, run relevant tests. If a test cannot be run, record the
  reason in the user response and in `dev/WORKLOG.md`.
- Integration tests that require TUN/root/sudo must be clearly separated from
  ordinary unit tests.
- For remote `fpshop` soak runs, build the Alpine runtime image locally and
  transfer it with `docker save | ssh ... docker load`. Do not build Docker
  images on weak remote soak hosts unless the user explicitly asks for a remote
  build experiment.

## Sudo And Environment

- The agent has passwordless sudo.
- Work happens in a Docker sandbox. Perform required system actions yourself
  when they are needed for the task.
- Even in the sandbox, do not damage the environment:
  - do not delete system directories;
  - do not overwrite host network configuration without need;
  - do not start endless load/stress jobs;
  - do not change firewall/routes globally without recording the current state
    and rollback plan;
  - do not leave background processes running when they are no longer needed.
- For TUN/routing integration tests, prefer isolated network namespaces when
  they are sufficient.

## Documentation

- Update `docs/specification.md` or the relevant document under `docs/` when an
  external contract, config, CLI, build/test workflow or architectural decision
  changes.
- The root `README.md` must stay a short welcome/index, not the main
  documentation.
- User and operator documentation should be written and maintained in English.
- Agent/developer Markdown files, except the root `AGENTS.md`, belong in
  `dev/`, not the repository root.
- Delete stale research/review artifacts when their conclusions have already
  been moved into current documents and further synchronization would be harmful.
- In documentation, distinguish MVP behavior, future work and experimental
  ideas.
- Code comments should explain non-trivial why, not obvious what.

## Agent Workflow

1. Read `AGENTS.md`, `docs/specification.md`, `dev/WORKLOG.md` if present and
   `git status --short`.
2. Provide a short plan for non-trivial tasks.
3. Make the minimum sufficient changes.
4. Run relevant checks.
5. Update `dev/WORKLOG.md`.
6. Inspect `git diff`.
7. Commit the logical change when the task reaches a stable point.
8. Tell the user what changed, how it was verified and what risks/questions
   remain.
