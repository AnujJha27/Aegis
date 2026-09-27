# Aegis

**A local control plane for coding agents.** Aegis keeps prompts, agent sessions, repository changes, verification, and handoff context together without replacing your editor or sending project data to an Aegis cloud service.

```text
Prompt → Agent → Observe → Changes → Verify → Review → Revise / Handoff
```

## Run Aegis

Build the frontend and native targets:

```bash
cd web
npm ci
npm run typecheck
npm test
npm run build
cd ..
cmake -S . -B build
cmake --build build -j2
```

Open a repository with the browser UI:

```bash
./build/aegis .
```

The launcher starts the local daemon, serves the built frontend, chooses an available loopback port, and opens the browser. It owns the daemon process and shuts it down when the launcher exits. In managed launch mode, closing the browser session also ends the daemon after a short reconnect grace period. WSL uses `wslview` or `cmd.exe` when available. `aegis --version` prints the build version and commit when known.

For development, run the daemon directly and use the Vite proxy:

```bash
./build/aegis_daemon --repo . --port 8080 --web-root web/dist
cd web && npm run dev
```

Direct daemon mode is not tied to a browser window. `aegis --daemon --repo . --port 8080 --web-root web/dist` is a launcher shorthand for the same mode.

## Architecture

```text
React + TypeScript + xterm.js
             │ HTTP / WebSocket
             ▼
        C++23 daemon
   ├── agent adapters and PTYs
   ├── task/run/event persistence
   ├── Git and verification
   └── loopback API server
```

The frontend owns presentation; the daemon owns domain state, process behavior, and persistence. An Aegis `Task` can contain multiple `AgentRun`s. Structured prompts and normalized output become semantic history; direct terminal keystrokes remain PTY transport and are not stored as individual input events. Codex runs retain the provider thread ID so later prompts resume the same conversation.

Review is a read-only code inspection workspace. It uses Monaco to render working-tree files and Git comparisons; it does not save or edit source. The agent writes, the human inspects, and requested revisions go back through the agent. File comparisons can show all changes (`HEAD ↔ WORKTREE`), staged changes (`HEAD ↔ INDEX`), or unstaged changes (`INDEX ↔ WORKTREE`).

See [docs/architecture.md](docs/architecture.md) for lifecycle states, interruption behavior, schema migration, API/event contracts, ownership, and security assumptions.

## Requirements and build options

The default build does **not** require Qt. It uses a C++23 compiler, CMake, SQLite, Boost headers, POSIX PTY support, and Threads. Node.js/npm are required to build the web bundle.

The old Qt prototype remains optional:

```bash
cmake -S . -B build-legacy -DAEGIS_BUILD_LEGACY=ON
cmake --build build-legacy -j2
./build-legacy/aegis_legacy --legacy-ui .
```

Its historical CLI utilities (`status`, `review`, `verify`, `analyze`, and others) are also available from `aegis_legacy` when built.

## Current workflow

- Create tasks and launch Codex, Claude Code, OpenCode, or a shell session.
- Continue Codex prompts in the same provider conversation; interrupt a turn without discarding a resumable run.
- Use xterm.js for interactive PTY sessions and CLI setup prompts.
- Inspect changed or repository files, compare staged/unstaged changes, stage or unstage in context, and restore verification evidence after reload.
- Jump from a task's file-change activity directly to the read-only diff; use `Ctrl/Cmd+P` to quick-open a repository file.
- Inspect bounded handoff context before switching agents.
- Stage, unstage, commit, push, pull, and switch local branches through the Git view.

The daemon serves only on `127.0.0.1`. HTTP request bodies are capped at 1 MiB, PTY WebSocket frames at 16 KiB, static assets at 16 MiB, and source file responses at 1 MiB by default (explicit load is capped at 8 MiB). File reads reject traversal and symlink escapes. Task data is stored in `<repository>/.aegis/aegis.sqlite`; data is sent to an external service only when the selected coding agent itself requires it.

## Build and install

For a self-contained Linux/WSL install directory:

```bash
./scripts/build-release.sh                 # writes ./dist/bin
./dist/bin/aegis /path/to/repository
```

Pass a different destination as the script's first argument. The release directory contains `aegis`, `aegis_daemon`, and the frontend bundle. CI runs the Linux build and tests on pushes and pull requests, plus a separate ASan/UBSan daemon test build. Native Windows is not supported; Linux and WSL are the supported environments.

Run `aegis --version` when reporting issues. The daemon also exposes `GET /api/version` with the schema version.
