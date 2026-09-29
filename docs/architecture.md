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

Task-scoped frontend snapshots are invalidated on task changes; late handoff, graph, provenance, verification, or verification-command responses cannot overwrite the currently selected task's state.

Persisted run statuses are `starting`, `running`, `completed`, `failed`, `interrupted`, and `terminated`. `finished_at` is zero only while a run is active. A Codex turn may complete while its resumable Aegis run remains `running`. PTY runs normally complete when their process exits. Explicit stop uses `terminated`; process failure uses `failed`. Runs left `starting` or `running` when the daemon starts are marked `interrupted`, since their owning processes no longer exist.

## Agent adapters and process ownership

The adapter contract exposes the capabilities currently needed: structured output, interactive PTY, resumability, and interruption. Provider parsing stays in `daemon/agents`; the frontend only consumes normalized `AgentEvent`s.

- **Codex** runs `codex exec --json` for the first prompt and records the `thread_id` from `thread.started`. Subsequent prompts use the installed CLI's `codex exec resume --json <thread_id> <prompt>` command. The generic `external_session_id` is persisted on the Aegis run.
- Each Codex run owns one idle worker that starts a cancellable child process per turn. A second prompt during a turn returns HTTP `409 run_busy`; request threads never join a previous turn.
- Interrupt sends SIGINT to the Codex process group. If needed, the process owner escalates to TERM/KILL after a short grace period. A `turn.interrupted` event leaves the run usable when its provider session ID exists. If interruption occurs before a resumable ID is received, the run becomes terminal `interrupted` rather than silently starting a fresh conversation on the next prompt.
- Claude Code, OpenCode, and shell use PTYs. xterm.js sends raw terminal frames through `/ws/pty/:run_id`; these bytes handle CLI prompts, menus, terminal control, and resizing.
- Text submitted through the Aegis semantic composer is recorded as `user.message`. Raw xterm.js keystrokes are transport only and are not inserted into task history. PTY output remains observable as `agent.message.delta` events.

The manager stores shared adapter ownership, releases its map lock before adapter operations, and joins adapters during termination/shutdown. The Codex process owner handles wait/reap; PTY reader threads are joined before their descriptors are closed. HTTP/WebSocket connection workers are tracked, sockets are shut down, and workers are joined when the server stops. No connection or adapter worker is detached.

Agent event and provider-session callbacks are exception-contained at adapter thread boundaries and log failures without emitting prompt/terminal contents. If SQLite itself rejects a write, that individual event/session ID may be lost, but the failure will not escape a worker thread and terminate the daemon.

## Persistence and migrations

Each repository stores data in `.aegis/aegis.sqlite`. SQLite access is serialized by the store mutex. `PRAGMA user_version` is the schema version; version 1 adds `runs.external_session_id` and the `verifications` table, version 2 adds persisted review findings, and version 3 indexes task/timestamp event queries. Migrations are explicit in `daemon/session/store.cpp`; databases newer than the binary's schema are rejected rather than downgraded.

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
| `verification.started`, `verification.completed` | Local check lifecycle; completion contains only the exit code, never command arguments or output |

Provider thread IDs are stored on the run and do not become display events. The event stream is for timeline/history, not for reconstructing all current domain state.

## Handoff bounds

`GET /api/tasks/:id/handoff` includes the original prompt, up to 20 recent events queried with SQL `LIMIT` (each content field capped at 4,000 bytes), up to 16,000 bytes of tracked Git patch plus safely-read untracked text snippets within a 24,000-byte total diff cap (binary and files over 1 MiB are represented by omission markers), up to 100 paths from Git status (including untracked and deleted files, with a truncation indicator), the latest verification with output capped at 8,000 bytes, and up to 20 review findings with each message capped at 2,000 bytes. The prompt is capped at 8,000 bytes. This is a reviewable context preview, not an automatic prompt injection.

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
| GET | `/api/files?path=...&scope=changed|all` | Lazy repository tree; `recursive=1` returns a bounded quick-open list; `include_changes=0` skips repeated Git status annotation when the caller already has a repository snapshot |
| GET | `/api/files/content?path=...&source=head|index|worktree` | One file at a Git/worktree source |
| GET | `/api/files/compare?path=...&base=head|index&target=index|worktree` | Structured old/new contents for a review diff |
| GET/POST | `/api/tasks/:id/findings` | List or add task findings with an optional run and line range |
| PATCH | `/api/findings/:id` | Set finding status to `open` or `resolved` |
| GET | `/api/version` | Build version, Git revision when available, and SQLite schema version |

`/ws/events` streams normalized semantic events. `/ws/pty/:run_id` is a separate interactive byte channel; resize frames are JSON control messages, while terminal text frames are written directly to the PTY.

## Read-only file review

`Files` is the repository-layer boundary for lazy tree enumeration and source reads. It uses Git-aware path enumeration, excludes `.git`, `.aegis`, and `node_modules`, and limits each listing to 500 entries. The frontend derives Changed Files from its current Git snapshot and overlays that same snapshot onto All Files, avoiding repeated status subprocesses and preventing stale scope responses from replacing the current tree. The All Files tree is cached until repository state changes; the selected file/diff is re-read whenever a fresh Git snapshot arrives, including after daemon reconnection. `HEAD`, `INDEX`, and `WORKTREE` are explicit internal sources; the interface labels them All Changes, Staged, and Unstaged. Rename comparisons use the old path when the base is `HEAD`, but keep the new path on both sides for `INDEX`↔`WORKTREE`. Missing sides (new/deleted files) are represented as empty content; binary content is identified without sending its bytes. Text is capped at 1 MiB by default and 8 MiB after an explicit user action.

Review findings are local persisted records containing task, optional run, repository-relative file, optional positive line range, message, status, and timestamps. The API validates task/run ownership, file path shape, line bounds, and message size. In Review, notes can be added to the selected file with an optional line/range and current run association, then resolved or reopened; task history reloads from the API. Handoff context includes at most 20 findings with each message capped at 2 KiB. Changed-file addition/deletion counts come from Git numstat records; binary files are marked separately, and untracked-file counts remain unavailable until Git tracks them.

Worktree reads validate repository-relative paths, canonicalize the target, and open it beneath the repository using no-follow path traversal. Git-object reads use argv-based Git commands and literal pathspecs. The React viewer is strictly read-only: Monaco has editor mutation disabled and only renders file contents or diffs. Monaco and its workers are bundled locally and the renderer loads only after a file is opened; no CDN is used. Review offers changed/all files, a small tab set, quick-open, inline/split diff, contextual staging, verification, and activity-to-file navigation. Task attribution is shown only when an event identifies the file; repository dirt is otherwise unowned.

HTTP request bodies are capped at 1 MiB, PTY WebSocket messages at 16 KiB, captured subprocess output at 16 MiB by default, verification output at 1 MiB, and static assets at 16 MiB. Git diff responses are captured at 1 MiB by default and handoff diffs at 24 KiB. File responses obey the source-size caps above. Diagnostics go to stderr and exclude prompts, PTY input, and environment credentials. SQLite initialization errors include the database path and do not overwrite failed databases.

## Launch and shutdown

`aegis .` remains the primary flow. The Qt-free C++ launcher resolves the repository, finds the sibling daemon and bundled frontend, starts the daemon on an automatically selected loopback port, reads its ready URL, and opens the browser. It owns the daemon child and terminates/reaps it on launcher exit. Managed launch additionally shuts the daemon down a few seconds after the last browser event WebSocket disconnects, allowing a page reload to reconnect. Direct `aegis_daemon --repo ...` mode remains available and does not auto-exit when no browser is connected.

The server binds explicitly to `127.0.0.1`. The default CMake build does not find or link Qt. `-DAEGIS_BUILD_LEGACY=ON` opts into the old Qt prototype and its legacy CLI utilities.

## Local-only security

- No Aegis accounts, cloud sync, or LAN binding.
- HTTP requests require exactly one loopback `Host` value, preventing DNS-rebinding hostnames from reaching local API or static routes.
- Browser WebSocket upgrades accept only loopback HTTP origins (`localhost`, `127.0.0.1`, or `[::1]`), covering the packaged UI and local Vite proxy while rejecting remote webpages. Origin-less native clients remain supported.
- Repository/session data stays local unless the selected external agent sends its own prompt to its service.
- Process commands use argv boundaries, not shell interpolation.
- Static serving walks paths relative to the configured frontend root without following symlinks, rejects traversal attempts, and caps assets at 16 MiB (including files that grow during a read).
- Verification requires a nonempty string-array command and an existing task; an optional run must belong to that task.
- File APIs reject absolute paths, `..`, NUL bytes, and resolved symlink escapes; Git paths are passed as argument-array values with literal pathspec handling.
- The launcher supports Linux/WSL only; the default build remains Qt-free, and release install places the daemon and static web bundle together under `bin/`.

## Deliberately deferred

Binary/Solidity analysis, LSP exploration, compiler AST/call graphs, risk heatmaps, investigation boards, time machine, and proposal comparison remain outside the current vertical slice. Review findings can be added, resolved, and reopened in the Review sidebar; Monaco remains read-only and does not render inline annotation decorations.

Browser-level Playwright E2E is deferred: no browser automation runtime is installed, and adding browser binaries plus cross-process fixture orchestration would materially expand CI. Focused frontend interaction tests, real-Git fixtures, and API route tests run in CI. The installed `aegis .` launch and local HTTP endpoints have been smoke-tested on Linux/WSL; do a visual browser inspection when reviewing the pending UI design.
