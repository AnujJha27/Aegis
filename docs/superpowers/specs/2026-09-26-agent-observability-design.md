# Agent observability and handoff design

## Goal

Add the four deferred slices to the existing Aegis vertical workflow without changing the core ownership boundary:

1. real PTY rendering with xterm.js;
2. structured handoff context;
3. a useful task/run/changes/verification graph;
4. lightweight provenance connecting output to runs, files, and checks.

The daemon remains the source of truth. React owns rendering and interaction. The feature set is local-first and must work without a cloud service.

## Backend design

### PTY events

Interactive adapters continue publishing normalized `agent.message.delta` events. The event content remains raw PTY bytes; the browser sends those bytes to xterm.js instead of displaying escape sequences as plain text. Structured Codex events continue rendering as semantic cards.

No agent-specific terminal parsing is added to React.

### Handoff context

Add a `HandoffContext` domain response composed from bounded, explicit sections:

- task prompt;
- selected recent events, capped by count and content size;
- current Git diff and changed paths;
- latest verification result when available;
- provenance records for the selected run.

Expose `GET /api/tasks/:id/handoff`. It returns a preview, not an automatic shell command. The user can inspect or copy it before starting another agent. This avoids injecting arbitrary previous text into an interactive shell or PTY.

### Graph response

Expose `GET /api/tasks/:id/graph`. The first graph is a domain graph, not a compiler call graph:

```text
Task → AgentRun → AgentEvent / ChangedFile → VerificationRun
```

Nodes and edges are structured JSON. Changed files come from the existing Git diff. React renders the graph with CSS/SVG, avoiding a graph library until real symbol-level analysis exists.

### Provenance

Expose `GET /api/tasks/:id/provenance`. The response links each persisted event to its task, run, agent, timestamp, and any changed paths that can be attributed to the run's current diff. Verification records link to the task and current changeset.

This is intentionally evidence-oriented and conservative: unknown attribution is represented as unknown rather than guessed.

## Frontend design

- Add `@xterm/xterm` and `xterm-addon-fit`.
- `AgentSession` owns the terminal instance and only writes PTY delta events to it.
- Semantic messages remain readable event cards below/alongside the terminal stream.
- The Review drawer gets functional Review, Graphs, Activity, and Handoff sections while staying hidden until requested.
- Graphs renders the daemon's node/edge response with native DOM/SVG.
- Activity displays provenance links and source badges.
- Handoff displays bounded context sections with copy-to-clipboard.

Loading states remain local to blocking requests. Window/browser sizing is never blocked by a feature request.

## API and error behavior

All new routes return structured JSON errors using the existing `{ error: { code, message } }` shape. Missing task IDs return `404`; malformed requests return `400`; unavailable Git/verification data is explicit rather than silently omitted.

The WebSocket remains the only live event transport. HTTP is used for snapshots such as graph, handoff, and provenance data.

## Testing and verification

Keep the existing focused daemon integration target as the only backend test addition. It will verify graph/handoff/provenance route shapes and PTY event normalization. Frontend verification remains strict TypeScript plus the production Vite build; no frontend test framework is added for these presentational slices.

## Non-goals

This slice does not implement compiler AST/call-graph extraction, multi-agent proposal comparison, automatic handoff execution, cloud sync, authentication, or a custom terminal emulator.
