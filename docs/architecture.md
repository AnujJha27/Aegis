# Aegis architecture

## Ownership

The React/TypeScript browser client presents tasks, agent output, terminals, changes, and verification. The C++23 daemon is authoritative for repository state, agent processes, normalized events, task/run lifecycle, and local persistence. New backend code uses standard C++ types; Qt is isolated to the optional legacy application.

```text
Browser UI ── HTTP requests / WebSocket events and PTY bytes ── C++ daemon
                                                                    ├─ agents
                                                                    ├─ Git / verification
                                                                    ├─ SQLite store
                                                                    └─ loopback HTTP server
```

## Task, run, and turn

```text
Task
├── initial prompt and repository
├── AgentRun (Codex, Claude, OpenCode, or shell)
│   ├── provider session ID when supported
│   ├── semantic user messages and normalized agent/tool events
│   └── one or more turns
├── verification history
└── bounded handoff snapshot
```

A task is the durable unit of work. Switching agents starts another run under the same task; it does not erase sibling-run history. A run is a logical adapter session, not a single prompt. Turns are represented by `turn.started`, `turn.completed`, and `turn.interrupted` events rather than by provider-specific frontend state.

Persisted run statuses are `starting`, `running`, `completed`, `failed`, `interrupted`, and `terminated`. `finished_at` is zero only while a run is active. A Codex turn may complete while its resumable Aegis run remains `running`. PTY runs normally complete when their process exits. Explicit stop uses `terminated`; process failure uses `failed`. Runs left `starting` or `running` when the daemon starts are marked `interrupted`, since their owning processes no longer exist.

## Agent adapters and process ownership

The adapter contract exposes the capabilities currently needed: structured output, interactive PTY, resumability, and interruption. Provider parsing stays in `daemon/agents`; the frontend only consumes normalized `AgentEvent`s.

- **Codex** runs `codex exec --json` for the first prompt and records the `thread_id` from `thread.started`. Subsequent prompts use the installed CLI's `codex exec resume --json <thread_id> <prompt>` command. The generic `external_session_id` is persisted on the Aegis run.
- Each Codex run owns one idle worker that starts a cancellable child process per turn. A second prompt during a turn returns HTTP `409 run_busy`; request threads never join a previous turn.
- Interrupt sends SIGINT to the Codex process group. If needed, the process owner escalates to TERM/KILL after a short grace period. A `turn.interrupted` event leaves the run usable when its provider session ID exists. If interruption occurs before a resumable ID is received, the run becomes terminal `interrupted` rather than silently starting a fresh conversation on the next prompt.
- Claude Code, OpenCode, and shell use PTYs. xterm.js sends raw terminal frames through `/ws/pty/:run_id`; these bytes handle CLI prompts, menus, terminal control, and resizing.
- Text submitted through the Aegis semantic composer is recorded as `user.message`. Raw xterm.js keystrokes are transport only and are not inserted into task history. PTY output remains observable as `agent.message.delta` events.

The manager stores shared adapter ownership, releases its map lock before adapter operations, and joins adapters during termination/shutdown. The Codex process owner handles wait/reap; PTY reader threads are joined before their descriptors are closed. HTTP/WebSocket connection workers are tracked, sockets are shut down, and workers are joined when the server stops. No connection or adapter worker is detached.

## Persistence and migrations

Each repository stores data in `.aegis/aegis.sqlite`. SQLite access is serialized by the store mutex. `PRAGMA user_version` is the schema version; version 1 adds `runs.external_session_id` and the `verifications` table while preserving existing task/run/event rows. Migrations are explicit in `daemon/session/store.cpp`; databases newer than the binary's schema are rejected rather than downgraded.

Verification records contain task ID, optional run ID, the command as a JSON argv array, exit code, output, and start/finish timestamps. Commands are executed as argument arrays, never converted to a shell string. Deleting a run removes its events; associated verification records retain task history and have their run association cleared by the SQLite foreign key.

## Normalized events

Events describe observable history; persisted run rows remain the source of run status truth.

| Event | Meaning |
| --- | --- |
| `run.started`, `run.completed`, `run.failed`, `run.interrupted`, `run.terminated` | Logical run lifecycle |
| `turn.started`, `turn.completed`, `turn.interrupted` | One prompt/response cycle inside a run |
| `user.message` | Semantic instruction submitted through Aegis |
| `agent.message.completed` | Normalized structured response |
| `agent.message.delta` | Interactive PTY output bytes |
| `command.started`, `command.completed` | Structured command activity when available |
| `file.changed` | Git path changed relative to the run's starting snapshot |

Provider thread IDs are stored on the run and do not become display events. The event stream is for timeline/history, not for reconstructing all current domain state.

## Handoff bounds

`GET /api/tasks/:id/handoff` includes the original prompt, up to 20 recent events (each content field capped at 4,000 bytes), a diff capped at 24,000 bytes, changed paths, and the latest verification with output capped at 8,000 bytes. The prompt is capped at 8,000 bytes. This is a reviewable context preview, not an automatic prompt injection. No review-finding model currently exists, so none is fabricated.

## HTTP and WebSocket API

The API uses structured errors:

```json
{"error":{"code":"run_busy","message":"an agent turn is already running"}}
```

| Method | Route | Purpose |
| --- | --- | --- |
| GET | `/api/health`, `/api/repository`, `/api/changes`, `/api/git/status` | Health and repository snapshots |
| GET | `/api/agents` | Adapter availability and capabilities |
| GET | `/api/tasks`, `/api/tasks/:id/runs`, `/api/tasks/:id/verifications` | Persisted task history |
| GET | `/api/events?task_id=...` | Persisted normalized event timeline |
| GET | `/api/tasks/:id/handoff`, `/graph`, `/provenance` | Review snapshots |
| POST | `/api/tasks` | Create a task |
| POST | `/api/tasks/:id/runs` | Start an agent run |
| POST | `/api/runs/:id/messages` | Submit a semantic message; returns `409 run_busy` during an active Codex turn |
| POST | `/api/runs/:id/interrupt`, `/terminate` | Interrupt a turn or stop a logical run |
| DELETE | `/api/runs/:id` | Delete a terminal run and its events |
| POST | `/api/verify` | Execute and persist an argv-array verification for a task and optional run |
| POST | `/api/git/stage`, `/unstage`, `/commit`, `/branch`, `/pull`, `/push` | Local Git actions |

`/ws/events` streams normalized semantic events. `/ws/pty/:run_id` is a separate interactive byte channel; resize frames are JSON control messages, while terminal text frames are written directly to the PTY.

## Launch and shutdown

`aegis .` remains the primary flow. The Qt-free C++ launcher resolves the repository, finds the sibling daemon and bundled frontend, starts the daemon on an automatically selected loopback port, reads its ready URL, and opens the browser. It owns the daemon child and terminates/reaps it on launcher exit. Managed launch additionally shuts the daemon down a few seconds after the last browser event WebSocket disconnects, allowing a page reload to reconnect. Direct `aegis_daemon --repo ...` mode remains available and does not auto-exit when no browser is connected.

The server binds explicitly to `127.0.0.1`. The default CMake build does not find or link Qt. `-DAEGIS_BUILD_LEGACY=ON` opts into the old Qt prototype and its legacy CLI utilities.

## Local-only security

- No Aegis accounts, cloud sync, or LAN binding.
- Repository/session data stays local unless the selected external agent sends its own prompt to its service.
- Process commands use argv boundaries, not shell interpolation.
- Static serving is limited to the configured frontend root and rejects traversal attempts.
- Verification requires a nonempty string-array command and an existing task; an optional run must belong to that task.

## Deliberately deferred

Binary/Solidity analysis, LSP exploration, compiler AST/call graphs, risk heatmaps, investigation boards, time machine, proposal comparison, and review findings remain outside the current vertical slice.
