# Aegis

## Local control for coding agents

Aegis is a local control plane for coding agents. It keeps the task, agent output, tool activity, repository changes, verification, and review context together while leaving the repository and subprocesses on your machine.

Aegis is not an IDE. Its core workflow is:

```text
Prompt → Agent → Observe → Changes → Verify → Review → Revise / Handoff
```

The first migration slice supports Codex, Claude Code, OpenCode, and a shell session behind one task-oriented web UI. The backend normalizes agent events so the frontend does not need agent-specific parsing.

## Quick start

Requirements:

- CMake 3.24+
- C++23 compiler
- Qt 6 Core and Widgets (kept for the legacy UI and CLI)
- Boost.Asio/Beast headers, SQLite3, and `libutil`
- Node.js 20+ and npm for the web bundle

Build the web bundle and C++ targets:

```bash
cd web
npm install
npm run build
cd ..
cmake -S . -B build
cmake --build build -j2
```

Launch the local control plane:

```bash
./build/aegis .
```

That starts `aegis_daemon` on `127.0.0.1`, serves `web/dist`, and opens the local UI in the default browser. The daemon chooses an available port automatically.

For frontend development, run the daemon separately and use Vite's proxy:

```bash
./build/aegis_daemon --repo . --port 8080 --web-root web/dist
cd web && npm run dev
```

The old Qt prototype remains available during migration:

```bash
./build/aegis --legacy-ui .
```

## Architecture

```text
Browser / React + TypeScript
        │  HTTP + WebSocket
        ▼
C++23 local daemon
  ├── task/session persistence (SQLite)
  ├── agent adapters and PTY lifecycle
  ├── Git and verification services
  └── normalized event stream
```

The frontend owns presentation. The daemon owns repository truth, process behavior, persistence, and event normalization. New daemon code uses standard C++ types; Qt remains isolated to the legacy application shell and existing prototype modules.

See [docs/architecture.md](docs/architecture.md) for the domain model, API, event protocol, persistence, launch flow, and local-only security assumptions.

## Current web workflow

- task list and task creation;
- agent availability and explicit agent launch;
- persistent task/run/event records;
- terminal/session output in the primary surface;
- one bottom composer for agent selection, status, prompt, send, and loading state;
- Review, Graphs, and Activity as a full-width secondary drawer;
- native SVG task/run/change graphs, provenance evidence rows, and bounded handoff context;
- current Git diff and configurable verification command;
- Codex JSON events and PTY output normalized at the daemon boundary.

Compiler-level analysis, Solidity/binary lenses, LSP exploration, architecture/call graphs, risk heatmaps, and proposal comparison remain deferred until the vertical slice is solid.

## CLI

Existing repository commands remain available:

```text
aegis status [path]
aegis review [path]
aegis verify [path] -- <command>
aegis analyze [path]
aegis lens [path]
aegis history [path]
aegis board [path]
aegis worktree [path]
aegis snapshot [path]
```

Useful daemon options:

```text
aegis_daemon --repo <path> --port <port> --web-root <path>
aegis --daemon --repo <path> --port <port> --web-root <path>
```

## Verification

Backend checks:

```bash
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Frontend checks:

```bash
cd web
npm run build
```

The migration keeps checks focused: one integration test covers each daemon boundary, while the frontend uses strict TypeScript and a production build rather than a second test framework.

## Local-only security

The daemon binds to loopback only and does not provide accounts, cloud storage, or LAN access. User-controlled commands cross process boundaries as argument vectors; they are not interpolated into shell strings. Task and event data are stored under the repository's `.aegis/aegis.sqlite` directory.
