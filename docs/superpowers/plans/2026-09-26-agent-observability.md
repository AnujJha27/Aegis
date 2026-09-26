# Agent Observability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add real PTY rendering, bounded handoff context, task graphs, and conservative provenance to the existing Aegis task workflow.

**Architecture:** Keep the daemon authoritative. Add small snapshot builders at the API boundary and keep graph/provenance as structured domain JSON. Use xterm.js only for raw PTY bytes; semantic events remain normal React cards.

**Tech Stack:** C++23, Boost.Beast, SQLite, React, TypeScript, Vite, `@xterm/xterm`, native DOM/SVG.

**Spec:** `docs/superpowers/specs/2026-09-26-agent-observability-design.md`

## Global Constraints

- Bind only to loopback.
- Keep new daemon code free of Qt types.
- Pass subprocess input as argument vectors or process data, never shell interpolation.
- Bound handoff content by count and size.
- Do not add a frontend test framework or duplicate daemon test binaries.
- Commit each task separately.

## Review Focus

- Missing task IDs return structured 404 errors.
- Empty or oversized handoff content remains bounded and safe.
- Raw PTY control bytes reach xterm.js rather than plain text cards.
- Graph/provenance snapshots remain useful when Git or verification data is unavailable.
- Browser cleanup and static production builds continue to work.

### Task 1: Backend snapshot models and routes

**Files:**
- Modify: `daemon/domain/types.h`, `daemon/protocol/json.h`
- Modify: `daemon/session/store.h`, `daemon/session/store.cpp`
- Modify: `daemon/api/routes.cpp`
- Modify: `tests/daemon_api_test.cpp`

- [ ] Add one failing API assertion for handoff, graph, and provenance snapshots.
- [ ] Implement bounded store accessors and route serializers using existing Git state/diff data.
- [ ] Return structured 404/500 errors for missing tasks and unavailable snapshots.
- [ ] Run the focused daemon integration test.
- [ ] Commit `feat: add handoff graph and provenance snapshots`.

### Task 2: Live xterm PTY surface

**Files:**
- Modify: `web/package.json`, `web/package-lock.json`
- Modify: `web/src/app/api.ts`, `web/src/app/events.ts`
- Modify: `web/src/features/terminal/AgentSession.tsx`
- Modify: `web/src/styles.css`

- [ ] Add xterm dependencies and wire a terminal ref with fit-on-resize.
- [ ] Route `agent.message.delta` content into xterm; keep structured events as cards.
- [ ] Preserve the plain-text fallback for non-PTY semantic output.
- [ ] Run `npm run build`.
- [ ] Commit `feat: render live agent PTY output`.

### Task 3: Graph, provenance, and handoff drawer views

**Files:**
- Modify: `web/src/app/api.ts`, `web/src/app/App.tsx`
- Modify: `web/src/components/Layout.tsx`
- Modify: `web/src/features/review/ReviewPanel.tsx`
- Modify: `web/src/styles.css`

- [ ] Load snapshots when the selected task or drawer view changes.
- [ ] Render graph nodes/edges with native DOM/SVG.
- [ ] Render provenance rows with run, agent, event, file, and verification evidence.
- [ ] Render bounded handoff sections with copy-to-clipboard feedback.
- [ ] Run `npm run build`.
- [ ] Commit `feat: add graph provenance and handoff views`.

### Task 4: Final verification and sync

**Files:**
- Modify: `docs/architecture.md`, `README.md`
- Modify: migration ledger

- [ ] Document the new endpoints and behavior.
- [ ] Run the full C++ build and CTest suite.
- [ ] Run the frontend production build.
- [ ] Run the daemon loopback/static smoke.
- [ ] Commit `test: verify agent observability slices` and push `main`.
