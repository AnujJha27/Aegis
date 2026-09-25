# Aegis Web Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a local C++23 daemon and React/TypeScript web UI that prove the task/agent-run/changeset workflow while keeping the existing prototype buildable during migration.

**Architecture:** Add a dependency-light daemon/ backend using standard C++ types, Boost.Beast for localhost HTTP/WebSocket transport, nlohmann/json at the API boundary, SQLite for local task/event persistence, and POSIX process/PTY adapters. Add a Vite React frontend under web/. Keep the existing Qt application available as a legacy path while the default aegis . flow launches the daemon and opens the local browser UI.

**Tech Stack:** C++23, CMake 3.24+, Boost.Asio/Beast, nlohmann/json, SQLite3, React, TypeScript, Vite, xterm.js only if needed for PTY rendering.

**Spec:** plan.md

## Global Constraints

- Backend remains C++23.
- New backend/domain code uses standard C++ types and does not depend on Qt.
- Server binds to loopback only.
- No cloud backend, accounts, authentication, Electron, or frontend-specific concepts in backend domain types.
- Keep existing CLI commands and Qt prototype buildable during migration.
- Use HTTP for request/response and WebSocket for normalized streaming events.
- aegis . remains the primary launch flow.
- Do not port advanced binary, Solidity, LSP, graph, risk, or proposal features into the first slice.
- Commit each completed task separately.

## Review Focus

- A missing or invalid repository path returns a structured HTTP error without crashing the daemon.
- A dead agent process produces an agent.failed or agent.finished event and leaves the task usable.
- WebSocket clients that disconnect do not block event publication or leak subscriptions.
- User prompts are passed as argument/vector or process input data, never interpolated into a shell command.
- The daemon remains loopback-only even when a port is supplied.
- Restarting the daemon does not lose persisted task/event records.

## Execution notes

The implementation keeps tests deliberately lean: the daemon store, API/static server, and process/agent services each have one integration target. The application startup path is exercised through the API target and direct executable smoke checks; no separate launcher or app test binary is added until that boundary has independent behavior worth testing.

---

### Task 1: Backend domain, SQLite store, and normalized event hub

**Files:**
- Create: daemon/domain/types.h
- Create: daemon/protocol/json.h
- Create: daemon/session/store.h
- Create: daemon/session/store.cpp
- Create: daemon/protocol/event_hub.h
- Create: daemon/protocol/event_hub.cpp
- Create: tests/daemon_store_test.cpp
- Modify: CMakeLists.txt

**Interfaces:**
- aegis::daemon::Task, AgentRun, AgentEvent, RepositoryState, VerificationRun.
- Store::open(path), createTask(prompt, repository), startRun(taskId, agent), appendEvent(event), tasks(), events(taskId).
- EventHub::publish(event) and EventHub::subscribe()/unsubscribe().

- [ ] Step 1: Write failing store tests covering task creation, run creation, event append/read, and persistence after reopening SQLite.
- [ ] Step 2: Run cmake --build build --target daemon_store_test and confirm the new test target fails because the backend module does not exist.
- [ ] Step 3: Implement the domain structs, SQLite schema, parameterized inserts, and event hub using std::string, std::vector, std::optional, and mutex-protected subscribers.
- [ ] Step 4: Run daemon_store_test and the existing CTest suite; confirm all pass.
- [ ] Step 5: Commit feat: add daemon task and event persistence.

### Task 2: Local HTTP/WebSocket transport

**Files:**
- Create: daemon/api/server.h
- Create: daemon/api/server.cpp
- Create: daemon/api/routes.h
- Create: daemon/api/routes.cpp
- Create: tests/daemon_api_test.cpp
- Modify: CMakeLists.txt

**Interfaces:**
- Server::start(port), Server::stop(), Server::port().
- GET /api/health, GET /api/repository, GET /api/tasks, POST /api/tasks.
- GET /api/events?task_id=... for persisted events.
- GET /ws/events for normalized runtime events.

- [ ] Step 1: Write failing API tests for health, repository metadata, task creation, invalid JSON, and loopback port binding.
- [ ] Step 2: Run the focused test and confirm failure because no server exists.
- [ ] Step 3: Implement a small Beast-based localhost server with one connection thread per request, JSON route handlers, and WebSocket subscriptions backed by EventHub.
- [ ] Step 4: Return consistent { "error": { "code": "...", "message": "..." } } errors and serialize domain types only in protocol/json.h.
- [ ] Step 5: Run API tests plus the full CTest suite.
- [ ] Step 6: Commit feat: add local HTTP and WebSocket API.

### Task 3: Repository, verification, and agent adapters

**Files:**
- Create: daemon/repository/git.h
- Create: daemon/repository/git.cpp
- Create: daemon/verification/runner.h
- Create: daemon/verification/runner.cpp
- Create: daemon/process/process.h
- Create: daemon/process/process.cpp
- Create: daemon/agents/adapter.h
- Create: daemon/agents/manager.h
- Create: daemon/agents/manager.cpp
- Create: daemon/agents/pty_adapter.h
- Create: daemon/agents/pty_adapter.cpp
- Create: daemon/agents/codex_adapter.h
- Create: daemon/agents/codex_adapter.cpp
- Create: tests/daemon_agents_test.cpp
- Modify: daemon/api/routes.cpp
- Modify: CMakeLists.txt

**Interfaces:**
- GitRepository::open(path), state(), diff().
- VerificationRunner::run(command, directory).
- AgentManager::available(), launch(taskId, agent), send(runId, message), interrupt(runId), terminate(runId).
- Adapters publish normalized AgentEvent values; the frontend never parses Codex/PTY output.

- [ ] Step 1: Write failing tests for agent capability listing, vector-based verification execution, and event normalization from one Codex JSONL message.
- [ ] Step 2: Run the focused test and confirm failure because adapters do not exist.
- [ ] Step 3: Implement safe argv-based command execution, repository state/diff retrieval, a bounded verification runner, and POSIX PTY lifecycle management without Qt.
- [ ] Step 4: Implement a generic PTY adapter for shell/Claude/OpenCode and a Codex adapter that converts JSONL records into normalized events.
- [ ] Step 5: Add API routes GET /api/agents, POST /api/tasks/:id/runs, POST /api/runs/:id/messages, POST /api/runs/:id/interrupt, GET /api/repository, GET /api/changes, and POST /api/verify.
- [ ] Step 6: Run focused tests and the full CTest suite; verify agent subprocesses are terminated on daemon shutdown.
- [ ] Step 7: Commit feat: add daemon agent and repository services.

### Task 4: Daemon executable and production static-file serving

**Files:**
- Create: daemon/main.cpp
- Create: daemon/app.h
- Create: daemon/app.cpp
- Create: tests/daemon_app_test.cpp
- Modify: daemon/api/server.cpp
- Modify: CMakeLists.txt

**Interfaces:**
- aegis_daemon --repo <path> --port <port> --web-root <path>.
- Default port 0 chooses an available port and prints the selected URL.
- Static / and /assets/* serve the built frontend from web/dist.
- API and WebSocket remain under /api and /ws.

- [x] Step 1: Cover daemon startup, selected port, health response, and static serving through the existing API integration test and executable smoke check; avoid a duplicate app test target.
- [ ] Step 2: Run the focused test and confirm failure because the daemon executable does not exist.
- [ ] Step 3: Wire Store, EventHub, GitRepository, VerificationRunner, AgentManager, and Server into an application object with explicit shutdown ordering.
- [ ] Step 4: Add static file serving with content-type mapping and path traversal rejection.
- [x] Step 5: Run daemon tests and the full suite.
- [x] Step 6: Commit feat: add standalone aegis daemon.

### Task 5: React/TypeScript frontend vertical slice

**Files:**
- Create: web/package.json
- Create: web/tsconfig.json
- Create: web/vite.config.ts
- Create: web/index.html
- Create: web/src/main.tsx
- Create: web/src/app/App.tsx
- Create: web/src/app/api.ts
- Create: web/src/app/events.ts
- Create: web/src/components/Layout.tsx
- Create: web/src/features/tasks/TaskList.tsx
- Create: web/src/features/agents/AgentPicker.tsx
- Create: web/src/features/terminal/AgentSession.tsx
- Create: web/src/features/review/ReviewPanel.tsx
- Create: web/src/features/verification/VerificationPanel.tsx
- Create: web/src/styles.css

**Interfaces:**
- api.ts owns HTTP requests and typed response models.
- events.ts owns WebSocket connection/reconnect and normalized event dispatch.
- Components consume domain-shaped frontend types and do not parse agent-specific output.

- [x] Step 1: Add the minimal Vite app and strict typecheck for repository/task/agent models.
- [x] Step 2: Install dependencies and run the production build.
- [ ] Step 3: Implement the three-area dark layout: task list, agent session/composer, and review panel.
- [ ] Step 4: Load repository metadata, tasks, and available agents from HTTP; create tasks and runs from the UI.
- [ ] Step 5: Connect WebSocket events and render normalized messages, tool activity, command activity, and lifecycle state.
- [ ] Step 6: Render current diff and verification results without implementing advanced graphs or editor behavior.
- [ ] Step 7: Run npm run build and daemon tests.
- [ ] Step 8: Commit feat: add React agent session frontend.

### Task 6: Preserve aegis . and add local launch flow

**Files:**
- Create: daemon/launcher.h
- Create: daemon/launcher.cpp
- Modify: src/main.cpp
- Modify: CMakeLists.txt
- Modify: README.md

**Interfaces:**
- aegis . validates the repository, starts aegis_daemon on loopback, waits for /api/health, and opens http://127.0.0.1:<port>/.
- aegis --legacy-ui . keeps the current Qt prototype available during migration.
- aegis --daemon ... forwards daemon arguments for development/debugging.

- [x] Step 1: Exercise executable resolution, loopback URL construction, invalid bundle handling, and child cleanup through the launch smoke path; avoid a duplicate launcher test target.
- [ ] Step 2: Implement launcher process startup, health polling with a timeout, browser opening through the platform default, and child cleanup on exit.
- [ ] Step 3: Route existing CLI commands unchanged and gate the old Qt window behind --legacy-ui.
- [ ] Step 4: Build the frontend, run the daemon, launch the browser flow in an offscreen/headless environment, and verify shutdown cleanup.
- [ ] Step 5: Update README and add docs/architecture.md.
- [x] Step 6: Commit feat: make aegis launch the local web control plane.

### Task 7: Final migration verification

**Files:**
- Modify: docs/architecture.md
- Modify: README.md
- Modify: docs/superpowers/plans/2026-09-25-aegis-web-migration.md

- [x] Step 1: Run the C++ build and full CTest suite.
- [x] Step 2: Run the frontend typecheck/build.
- [x] Step 3: Start the daemon against the repository and exercise health, static serving, task API, agent listing, and service paths through focused integration checks.
- [x] Step 4: Confirm the daemon binds only to 127.0.0.1, rejects traversal, persists task/events, and terminates child agents.
- [x] Step 5: Perform a self-review against the spec and record deferred advanced features.
- [x] Step 6: Record final migration verification in the repository ledger.
