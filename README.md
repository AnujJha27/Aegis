# Aegis

### Agent-native software engineering under human control.

Aegis is a local-first control plane for coding agents. It keeps the agent session in the foreground, then adds the context that is usually missing: repository state, structured output, reviewable diffs, analysis, verification, and an activity trail.

It works with the tools you already use instead of trying to replace them.

## Why Aegis exists

Agent workflows are fast, but the result is often difficult to supervise. Aegis sits between the agent and the repository so changes stay:

- observable — see prompts, output, commands, and session activity;
- attributable — know which agent action produced a result;
- reviewable — inspect the actual diff and affected files;
- verifiable — run tests and checks from the same workspace;
- local-first — keep session and project data on your machine.

The default experience is intentionally simple: launch Aegis, work with an agent, and open review surfaces only when you need them.

## Highlights

| Surface | What it provides |
| --- | --- |
| Agent session | Shell, Codex, Claude, or OpenCode sessions from one window |
| Structured transcript | Readable cards for prompts, agent messages, tools, and errors |
| Review drawer | Unified diff, evidence, symbols, architecture, risk, history, and activity |
| Repository analysis | Changed files, dependencies, findings, tests, blast radius, AST, and call graph data when available |
| Verification | Project tests plus optional LSP, formal-methods, and Solidity checks |
| Session control | Worktrees, snapshots, handoffs, critic mode, and proposal comparison |
| CLI | Status, review, analysis, history, board, worktree, snapshot, and verification commands |

## Quick start

### Requirements

- CMake 3.24 or newer
- A C++23 compiler
- Qt 6 with Core and Widgets modules
- `libutil` and a POSIX-style terminal environment for the managed terminal session

Agent executables are optional at build time. Install whichever backends you want to use and make sure they are available on `PATH`.

### Build

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

### Launch the GUI

```bash
# Open the current repository with the default shell agent
./build/aegis .

# Start with a specific backend
./build/aegis . --agent codex
./build/aegis . --agent claude
./build/aegis . --agent opencode
```

Use `--paranoia` when you want to reduce session persistence:

```bash
./build/aegis . --paranoia
```

## Working in the GUI

The session/terminal is the primary surface. The composer stays at the bottom so the current agent, prompt, status, and send action remain close together.

| Shortcut | Action |
| --- | --- |
| `Ctrl+R` | Open or close the review drawer |
| `Ctrl+P` | Quick-open a changed file |
| `Ctrl+Shift+P` | Open the command palette |
| `Ctrl+Shift+O` | Open the evidence view |
| `Esc` | Close the review drawer |
| `Enter` | Send the current prompt |

The review area is intentionally secondary. Use it when you need to inspect the work, understand impact, or run a check; return to the session when you are done.

## CLI

Every command accepts an optional repository path. If omitted, Aegis uses the current directory.

```text
aegis status [path]                 Show branch and working-tree summary
aegis review [path]                 Print the current diff
aegis analyze [path]                Analyze changes and impact
aegis lens [path]                   Print specialized analysis lenses
aegis history [path]                Show Aegis events and recent Git history
aegis board [path]                  Show the session board
aegis worktree [path]               Create a detached review worktree
aegis snapshot [path]               Save the current diff as a patch
aegis verify [path] -- <command>    Run a verification command
```

Examples:

```bash
./build/aegis status .
./build/aegis analyze .
./build/aegis verify . -- ctest --test-dir build
./build/aegis snapshot .
```

When no verification command is supplied, `aegis verify` runs:

```bash
ctest --test-dir build
```

## How the project is organized

The code is split into small libraries around the application shell:

```text
include/aegis/       Public module headers
src/main.cpp         GUI bootstrap and CLI dispatch
src/commands.cpp     Process and Git command execution
src/window.cpp       Qt application window and session coordination
src/transcript.cpp   Codex JSONL event parsing
src/analysis.cpp     Repository and change analysis
src/session.cpp      Local session events, board, and persistence
src/terminal.cpp     Managed PTY process session
src/ui.cpp           Qt styling and reusable UI helpers
tests/               Focused module tests
```

The application is built with CMake and Qt 6. Core behavior is kept outside the window layer so the CLI and tests can reuse it without starting the GUI.

## Agent backends

Aegis currently recognizes these launch targets:

- `shell`
- `codex`
- `claude`
- `opencode`

For Codex, Aegis uses non-interactive JSON output and preserves the conversation thread for follow-up prompts. Other agents are managed through the terminal session and receive normal line-oriented prompts.

## Testing

Run the full suite with:

```bash
ctest --test-dir build --output-on-failure
```

The suite covers core summaries, repository analysis, session persistence, terminal behavior, UI helpers, transcript parsing, and CLI smoke commands.

## Project status

Aegis is an active prototype. The main workflow is functional, but analysis quality depends on the tools available in the repository environment and on the language being inspected. Missing compilers, language servers, formal tools, or agent executables are reported rather than bundled.

Contributions should favor small, testable modules and preserve the session-first workflow. Build and test locally before committing changes.
