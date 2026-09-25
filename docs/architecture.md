# Aegis architecture

## Responsibility split

The browser is a presentation client. It fetches domain data, sends user intent, renders normalized events, and controls the current task/run view.

The local C++ daemon is the authority. It opens the repository, starts agents, owns PTYs and subprocesses, runs Git and verification commands, persists task history, and publishes normalized runtime events.

The legacy Qt application is a migration shell, not a dependency of the new daemon domain. `--legacy-ui` keeps it available while the browser workflow becomes the default.

## Domain model

```text
Task
├── prompt and repository
├── AgentRun
│   ├── normalized messages
│   ├── tool / command activity
│   └── lifecycle state
├── repository changes
├── VerificationRun
└── review / handoff context
```

The current daemon stores `Task`, `AgentRun`, and `AgentEvent` records. `RepositoryState` and `VerificationRun` are returned as structured API data. Future review and handoff records can extend the same task without changing the frontend into an agent-specific parser.

## Backend modules

```text
daemon/
├── agents/       adapter interface, Codex JSON adapter, PTY adapter, manager
├── api/          Beast HTTP/WebSocket transport and request routes
├── domain/       standard-library domain structs
├── process/      argv-safe POSIX process execution
├── protocol/     JSON boundary and in-process event hub
├── repository/   Git state and diff retrieval
├── session/      SQLite task/run/event persistence
├── verification/ bounded argv-safe verification execution
└── app.*         explicit application ownership and shutdown
```

New backend modules should use `std::string`, `std::vector`, `std::filesystem`, `std::chrono`, and `std::optional`. Qt types belong at the legacy UI boundary only.

## HTTP API

The API is intentionally small:

| Method | Route | Purpose |
| --- | --- | --- |
| GET | `/api/health` | daemon health |
| GET | `/api/repository` | branch and change summary |
| GET | `/api/changes` | current diff |
| GET | `/api/agents` | available adapters and capabilities |
| GET | `/api/tasks` | persisted tasks |
| GET | `/api/events?task_id=...` | persisted task events |
| POST | `/api/tasks` | create a task |
| POST | `/api/tasks/:id/runs` | launch an agent run |
| POST | `/api/runs/:id/messages` | send a prompt/message |
| POST | `/api/runs/:id/interrupt` | interrupt a run |
| POST | `/api/verify` | run an argument-array verification command |

Errors use one shape:

```json
{
  "error": {
    "code": "missing_prompt",
    "message": "prompt is required"
  }
}
```

## WebSocket event protocol

`GET /ws/events` upgrades to a WebSocket. Events are normalized before they reach the browser:

```json
{
  "id": "event-7",
  "task_id": "task-42",
  "run_id": "run-3",
  "type": "agent.message.completed",
  "agent": "codex",
  "content": "Implemented the requested change.",
  "timestamp": 1727000000
}
```

The first slice uses lifecycle, message, command, and failure events. Raw PTY control sequences are presentation noise and are cleaned at the UI edge; agent-specific JSON parsing stays in the daemon adapter.

## Agent adapters

The manager exposes capabilities and owns active runs. Structured agents use their machine-readable output when available. Interactive agents use a POSIX PTY and receive prompt input as PTY data. Both paths publish the same `AgentEvent` shape.

An agent switch creates another run under the same task. The task history therefore survives switching instead of being hidden inside a terminal widget. Handoff context will be built from selected task prompt, recent events, current diff, verification failures, and review findings rather than an unbounded transcript dump.

## Persistence and lifecycle

Each opened repository gets `.aegis/aegis.sqlite`. The daemon initializes its schema on startup and writes tasks, runs, and events synchronously behind a small store mutex. EventHub subscribers are independent queues, so a disconnected WebSocket cannot block persistence or another client.

`aegis .` owns the daemon process through the Qt launch shell. Shutdown terminates the child daemon, and the daemon manager terminates active agent subprocesses. Direct daemon use is also supported for development and automation.

## Local-only assumptions

- the server binds explicitly to `127.0.0.1`;
- no authentication or cloud account is involved;
- repository/session data stays local unless the selected external agent itself sends a prompt to its own service;
- user input is passed as argument arrays or process input, never shell-interpolated;
- static file serving rejects traversal attempts and only serves the configured frontend root.

## Deliberately deferred

Binary analysis, Solidity views, LSP exploration, architecture/call graphs, risk heatmaps, investigation boards, time machine, multi-agent proposal comparison, advanced provenance, and xterm.js fidelity are not part of the first migration slice. The task/run/event boundaries leave room for them without putting their rendering concepts into the daemon core.
