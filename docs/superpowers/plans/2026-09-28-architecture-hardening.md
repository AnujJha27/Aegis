# Aegis architecture hardening

## Goal

Make persisted task/run state authoritative, keep agent turns cancellable and nonblocking, and align API, UI, build, and documentation with the existing local C++23 daemon + React architecture.

## Repository findings

- `Store` has no schema version; runs stay `starting`; verification is transient.
- Codex uses one-shot `exec` per message, blocks joining its previous worker, and has a no-op interrupt. The installed CLI supports `codex exec resume`; legacy Aegis already parsed/preserved Codex thread IDs.
- `Manager` calls adapters under a shared mutex; server connection threads are detached; PTY and Codex emit legacy `agent.*` lifecycle names.
- Routes and nearly all app state live in single files; the browser derives completion from events and does not restore verification.
- CMake requires Qt and builds the old Qt launcher by default, although the daemon and browser do not require it.
- Preserve the isolated `.aegis-git` repository; do not use the parent workspace Git repository.

## Decisions

- Keep one Aegis `AgentRun` across multiple turns; persist the adapter's generic external session ID.
- Persist run states `starting`, `running`, `completed`, `failed`, `interrupted`, `terminated`; represent turns with normalized events. Interrupt cancels the current Codex process/turn and leaves the resumable run `running`; prompt submission during an active turn returns `409 run_busy`.
- Persist verification command as an argv array and associate it with task plus optional run.
- Keep raw PTY frames out of event history; semantic composer messages are persisted as `user.message`.
- Keep changes incremental and use the existing focused C++ integration tests and frontend typecheck/build.

## Implementation sequence

1. Add SQLite `user_version` migrations, run lifecycle/provider session persistence, and verification history; extend the existing Store test.
2. Make process execution cancellable and adapters asynchronous; persist lifecycle/turn events, Codex resume IDs, semantic messages, and explicit busy errors; fix manager/process/PTY/server ownership.
3. Persist/restore verification and include bounded latest verification in handoff; split routes and centralize path parsing/error handling; extend existing API integration coverage.
4. Extract lightweight frontend state hooks and render run lifecycle/verification snapshots from persisted API state.
5. Make Qt opt-in and provide a non-Qt `aegis .` launcher; keep Qt prototype behind `AEGIS_BUILD_LEGACY`.
6. Update README/architecture and run focused integration, full C++ build/CTest, frontend checks, and launch/API smoke checks.

## Review focus

- Interrupted Codex turn is cancelled but its run remains resumable when a thread ID exists.
- No request handler joins or waits for an active agent turn.
- Shutdown owns and joins all connection, adapter, and reader threads without holding locks across joins/callbacks.
- Existing SQLite data migrates in place; verification argv boundaries and loopback-only binding remain intact.
- Default CMake configure/build succeeds without Qt, and `aegis .` still launches the browser workflow.
