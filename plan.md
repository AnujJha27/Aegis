# AEGIS

## Product Requirements Document

### Working tagline

**Agent-native software engineering under human control.**

Alternative internal positioning:

> Aegis is the control, trace, review, and verification layer between coding agents and your codebase.

---

# 1. Product Summary

Aegis is a local-first software engineering environment designed for workflows in which AI coding agents perform an increasing share of code generation and modification.

Aegis is not primarily:

* a code editor,
* an IDE,
* an AI chatbot,
* a TUI clone of VS Code,
* or a separate review application that the user constantly switches into.

Instead, Aegis sits **around the coding agent session itself**.

The user launches:

```bash
aegis .
```

Aegis then launches or attaches to a coding environment such as:

```text
Codex
Claude Code
OpenCode
Shell
```

The user continues interacting with the selected agent normally.

Aegis remains largely invisible until useful.

While the agent works, Aegis records and understands:

* repository changes,
* commands executed,
* processes spawned,
* files touched,
* Git state,
* tests run,
* build/lint/typecheck results,
* security findings,
* affected symbols,
* dependency changes,
* agent prompts and revisions,
* provenance of generated changes.

When requested, Aegis exposes review and investigation surfaces without forcing the user into a separate workflow.

The central philosophy is:

> **Do not replace the coding agent. Wrap it with understanding, control, and evidence.**

---

# 2. Core Product Idea

Today, agent-assisted development often looks like:

```text
Developer
   ↓
Coding Agent
   ↓
Repository changes
   ↓
Developer manually inspects everything
```

Aegis changes this to:

```text
Developer
   ↓
AEGIS
   ↓
Coding Agent
   ↓
Repository changes
   ↓
AEGIS builds trace + evidence + impact model
   ↓
Developer reviews with context
```

Aegis should make machine-written software easier to supervise without slowing down the agent workflow.

---

# 3. Core Principles

## 3.1 The agent remains the primary interaction surface

Aegis must not force users to abandon tools they already like.

The user should still feel like they are using:

```text
Codex
Claude Code
OpenCode
Shell
```

Aegis wraps those tools.

It should not recreate their conversational interfaces unnecessarily.

---

## 3.2 Stay invisible until useful

During normal work, Aegis should occupy minimal visual space.

A typical session may show only:

```text
+4 files   +117 -62   tests ✓   findings 1   REVIEW ^R
```

More information appears only when requested.

---

## 3.3 Human control over autonomous actions

The agent may act quickly.

Aegis ensures those actions remain:

* observable,
* attributable,
* reversible,
* reviewable,
* verifiable.

---

## 3.4 Evidence over summaries

Aegis should not merely tell the user:

> “The change looks safe.”

Instead, it should expose evidence such as:

```text
238 tests passed
1 integration test failed
3 public symbols changed
new dependency added
2 call paths affected
no new compiler diagnostics
1 new external call
```

Aegis may summarize, but evidence must remain directly inspectable.

---

## 3.5 Local-first

Aegis itself should require no cloud backend.

Project data remains local.

External coding agents and AI services may be used according to user configuration.

---

## 3.6 Agent-agnostic

Aegis must never depend fundamentally on one AI provider.

Agents are external execution backends.

---

## 3.7 Native tool, terminal-first workflow

Aegis should be launched from the terminal.

Primary interface:

```bash
aegis .
```

The default workflow should remain terminal-centric.

Graphical surfaces should appear only for tasks that benefit meaningfully from richer visualization.

---

# 4. User Experience Model

Aegis has two primary surfaces.

## 4.1 Aegis Shell

The main experience.

This is where the agent session runs.

Example:

```text
╭─ AEGIS / CODEX ─────────────────────────────────────────────╮
│ project: jim2                  branch: feature/agents       │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│ > refactor the parser to stream input rather than buffer    │
│   the whole file                                            │
│                                                             │
│ • Reading parser.cpp                                        │
│ • Reading parser_test.cpp                                   │
│ • Editing parser.cpp                                        │
│ • Running tests                                             │
│                                                             │
│ Implemented incremental parsing and updated tests.          │
│                                                             │
├─────────────────────────────────────────────────────────────┤
│ +3 files  +91 -35   tests ✓   risk 1   REVIEW ^R           │
╰─────────────────────────────────────────────────────────────╯
```

Aegis should not redraw or imitate the agent unnecessarily.

Where possible, the agent process should run naturally inside Aegis-managed terminal infrastructure.

---

# 5. Aegis Deep View

Some problems do not fit comfortably inside a terminal interface.

Examples:

* large side-by-side diffs,
* architecture graphs,
* call graphs,
* storage layouts,
* binary analysis,
* dependency maps,
* large blast-radius views.

For these cases Aegis may open a native graphical surface from the same executable.

Example shortcut:

```text
Ctrl+Shift+O
```

or command:

```bash
aegis inspect
```

This is not a separate product.

It is an extended visual surface of the current Aegis session.

---

# 6. Technology Direction

Preferred implementation:

```text
C++23
Qt 6
```

Primary components:

```text
Aegis Core
├── Session Manager
├── Process Supervisor
├── Repository Monitor
├── Git Engine
├── Diff Engine
├── Symbol Engine
├── LSP Manager
├── Agent Adapter Layer
├── Verification Engine
├── Security Engine
├── Provenance Engine
├── Snapshot Engine
└── Trace Database
```

The terminal experience should use a capable embedded terminal implementation rather than attempting to manually emulate every terminal behavior.

The graphical surface may use:

```text
Qt Quick / QML
```

for presentation while keeping repository, process, analysis, Git, and agent logic in C++.

No browser runtime should be required.

No Electron dependency.

No TypeScript requirement.

---

# 7. Core Session Model

Every Aegis invocation creates an:

```text
Aegis Session
```

A session represents a period of software work.

Example structure:

```text
Session
├── repository
├── starting Git state
├── agent runtime
├── agent conversations/tasks
├── shell commands
├── file events
├── verification runs
├── findings
├── snapshots
└── final Git state
```

Sessions may be persisted locally unless privacy settings prevent it.

---

# 8. Agent Adapters

Agents must be implemented through adapters.

Conceptual interface:

```cpp
class AgentAdapter {
public:
    start();
    sendInput();
    interrupt();
    terminate();
    getStatus();
    getCapabilities();
};
```

Initial adapters:

```text
Codex
Claude Code
OpenCode
Generic command
Plain shell
```

Adding new agents should not require changing the rest of Aegis.

---

# 9. Agent Launch Flow

When the user runs:

```bash
aegis .
```

Aegis should either:

1. launch the user's preferred default agent, or
2. show a minimal runtime selector.

Example:

```text
AEGIS

> Codex
  Claude Code
  OpenCode
  Shell
```

Configuration should allow:

```bash
aegis . --agent codex
```

and aliases such as:

```bash
aegis codex .
```

---

# 10. Status Strip

The status strip is one of Aegis's most important UI elements.

It should remain visible without becoming noisy.

Example:

```text
+4 files   +117 -62   tests ✓   findings 1   branch feature/auth   REVIEW ^R
```

Possible signals:

```text
Files changed
Lines changed
Git state
Verification status
Agent state
Security findings
Snapshot state
Network state
Current branch
Current task
```

The strip should be configurable but sensible by default.

---

# 11. Repository Monitoring

Aegis should continuously monitor repository state during the session.

Track:

* file creates,
* modifications,
* deletions,
* renames,
* permission changes,
* Git metadata,
* dependency manifests,
* lockfiles.

Events should be debounced and aggregated.

Aegis should distinguish:

```text
agent-originated change
external change
human direct edit
verification artifact
build artifact
```

when possible.

---

# 12. Change Capsules

Every meaningful coding task should produce a **Change Capsule**.

A Change Capsule contains:

```text
Task
Prompt
Agent
Start timestamp
End timestamp
Starting repository state
Files changed
Diff
Symbols affected
Commands executed
Tests run
Diagnostics
Security findings
Dependencies changed
Follow-up prompts
Final outcome
```

Change Capsules become the atomic unit of agent history.

Example:

```text
Capsule #42

Task:
Replace JWT parsing with streaming verification.

Agent:
Codex

Files:
4

Diff:
+117 -62

Verification:
Build ✓
Unit ✓ 238/238
Integration ✗ 11/12

Risk:
1 new external call

Status:
Needs review
```

---

# 13. Review Mode

Shortcut:

```text
Ctrl+R
```

Review mode should expand beside or over the existing agent session.

Example:

```text
╭─ CODEX ─────────────────────────╮╭─ AEGIS REVIEW ─────────────╮
│                                 ││                            │
│ > refactor parser...            ││ 4 files changed            │
│                                 ││ +117 -62                   │
│ Done.                           ││                            │
│                                 ││ parser.cpp     +74 -40     │
│                                 ││ lexer.cpp      +21 -9      │
│                                 ││ tests.cpp      +22 -13     │
│                                 ││                            │
│                                 ││ Build       ✓              │
│                                 ││ Tests       ✓              │
│                                 ││ Findings    1              │
╰─────────────────────────────────╯╰────────────────────────────╯
```

Review mode should support keyboard-first navigation.

---

# 14. Diff Review

Required diff modes:

```text
Unified
Side-by-side
File summary
Symbol-level
```

Features:

* syntax highlighting,
* hunk navigation,
* file navigation,
* changed-line counts,
* staged/unstaged distinction,
* direct copy,
* open in editor,
* ask agent about selected hunk.

---

# 15. Semantic Diff

Aegis should eventually go beyond line-level changes.

Examples:

```text
Function signature changed
Parameter removed
Return type changed
Condition reversed
New exception path
Loop introduced
Dependency added
State mutation added
Public method removed
External API call added
```

This feature can begin heuristically.

Later versions may use:

* AST comparison,
* Tree-sitter,
* LSP,
* compiler metadata.

---

# 16. Blast Radius

Aegis should estimate what a change can affect.

For a selected symbol or changeset:

```text
BLAST RADIUS

authenticateUser()

17 callers
4 modules
3 tests
2 public APIs
1 database boundary
```

Sources may include:

* LSP references,
* import graph,
* call graph,
* build dependencies,
* test references,
* package relationships.

The interface should allow drilling into each category.

---

# 17. Provenance

Aegis should track **how code entered the repository**.

Possible labels:

```text
Human
Codex
Claude Code
OpenCode
Imported commit
Unknown
```

Example:

```text
src/parser.cpp:84-118
Generated by Codex
Session #42
2026-09-19
```

The aim is not perfect attribution.

The aim is usable agent-era provenance.

---

# 18. Agent Trace

Aegis should provide an inspectable trace of the task.

Example:

```text
TRACE

09:41:12  read src/parser.cpp
09:41:16  read include/parser.h
09:41:22  exec rg "parseToken"
09:41:34  modify src/parser.cpp
09:41:48  exec cmake --build build
09:42:03  exec ctest
09:42:11  modify tests/parser_test.cpp
09:42:23  exec ctest
09:42:32  completed
```

Exact trace fidelity depends on agent integration.

At minimum Aegis should capture:

* process output,
* filesystem changes,
* commands visible through the managed environment.

---

# 19. Agent Worktrees

Aegis should support running agent tasks inside isolated Git worktrees.

Example:

```text
Run task in:

○ Current workspace
● Isolated Aegis worktree
```

Benefits:

* agent cannot unexpectedly pollute the main working tree,
* multiple implementations can exist simultaneously,
* rollback becomes trivial,
* proposals become first-class artifacts.

At task completion:

```text
Merge
Cherry-pick
Apply patch
Request revision
Discard
```

---

# 20. Multi-Agent Proposals

A major future feature.

The user may submit the same task to multiple agents.

Example:

```text
Task:
Replace custom parser with incremental parser.

Run with:

✓ Codex
✓ Claude Code
□ OpenCode
```

Each runs independently.

Aegis presents:

```text
Proposal A — Codex
+91 -42
238 tests passing
1 new dependency

Proposal B — Claude
+63 -18
238 tests passing
0 new dependencies
```

The user chooses based on evidence rather than agent reputation.

---

# 21. Revision Workflow

A user reviewing a change should be able to select:

```text
file
hunk
symbol
finding
test failure
```

and send a targeted revision request.

Example:

```text
This branch duplicates validation already performed in parseHeader().
Reuse that path instead.
```

Aegis should preserve the original task context and attach the revision to the same Change Capsule.

---

# 22. Verification Engine

Projects should support configurable verification pipelines.

Common steps:

```text
Build
Unit tests
Integration tests
Lint
Format check
Typecheck
Static analysis
Security scan
Custom command
```

Aegis should detect common ecosystems.

Examples:

```text
CMakeLists.txt
Makefile
Cargo.toml
go.mod
package.json
pyproject.toml
foundry.toml
pom.xml
build.gradle
```

Detected commands must remain editable.

---

# 23. Verification Recipes

A repository may define reusable recipes.

Example:

```text
fast
├── compile
├── unit tests
└── lint

full
├── clean build
├── unit tests
├── integration tests
├── static analysis
└── security suite

audit
├── forge test
├── forge invariant
├── slither
└── custom scripts
```

Commands may run automatically after an agent task if configured.

---

# 24. Evidence View

Review should culminate in evidence.

Example:

```text
EVIDENCE

Build
✓ successful

Tests
✓ unit 238/238
✗ integration 11/12

Diagnostics
✓ no new compiler warnings

API
⚠ 1 public signature changed

Dependencies
✓ unchanged

Security
⚠ new external call in auth.cpp

Blast radius
17 callers
3 affected tests
```

Each item must be drillable.

---

# 25. Test Impact Analysis

Aegis should identify tests related to changed code.

Possible strategies:

* static references,
* naming conventions,
* previous test runs,
* coverage data when available.

Example:

```text
Likely affected tests

auth_test.cpp
session_test.cpp
login_integration.cpp
```

Future versions may automatically prioritize these tests before running full suites.

---

# 26. Dependency Monitoring

Dependency changes should be highly visible.

Watch files such as:

```text
package.json
package-lock.json
Cargo.toml
Cargo.lock
go.mod
go.sum
requirements.txt
pyproject.toml
poetry.lock
CMakeLists.txt
conanfile
vcpkg.json
```

Aegis should report:

```text
DEPENDENCY CHANGE

Added:
libsodium 1.0.20

Removed:
custom-crypto

Lockfile:
+182 lines
```

Dependency additions should never disappear inside a large diff.

---

# 27. Security Lens

Aegis should include a generic security analysis layer.

Initial checks may include:

* hardcoded secrets,
* unsafe command execution,
* dangerous C/C++ calls,
* obvious path traversal patterns,
* insecure temporary-file usage,
* suspicious permission changes,
* network exposure changes,
* unsafe deserialization patterns.

Results should be attached to the exact change that introduced them when possible.

---

# 28. Solidity Lens

Aegis should eventually inherit the strongest security concepts from Jim.

Capabilities:

```text
Contract map
Inheritance graph
External call graph
Storage layout
Storage collision analysis
Modifier map
Privileged functions
State-changing functions
delegatecall usage
tx.origin usage
low-level calls
unchecked calls
assembly/Yul regions
memory pointer inspection
gas hotspots
Foundry test status
Foundry invariant status
```

Example:

```text
SOLIDITY LENS

Vault.sol

Privileged entrypoints: 4
External calls: 6
delegatecall: 0
Assembly blocks: 2

Storage
slot 0 owner
slot 1 treasury
slot 2 totalAssets
slot 3 balances mapping
```

---

# 29. Binary Lens

Future binary capabilities:

```text
ELF
PE
Mach-O
```

Possible tools:

* headers,
* sections,
* symbols,
* imports,
* exports,
* strings,
* hashes,
* disassembly,
* hex view,
* entrypoint visualization.

Binary analysis should appear only when relevant.

---

# 30. Architecture View

Aegis Deep View should support visual repository architecture.

Possible nodes:

```text
packages
modules
files
classes
contracts
services
```

Possible edges:

```text
imports
calls
inherits
depends on
publishes to
reads from
writes to
```

The graph should remain useful rather than decorative.

Prefer aggregation at high zoom levels.

---

# 31. Risk Heatmap

Aegis should calculate repository hotspots.

Possible signals:

```text
recent churn
complexity
agent modifications
security findings
test failures
dependency depth
number of callers
historical bug density
```

Example:

```text
src/auth/        █████████
src/parser/      ██████
src/ui/          ██
docs/            ░
```

Risk should be treated as a heuristic signal, not an authoritative judgment.

---

# 32. Investigation Board

Users may pin items during debugging, auditing, or review.

Pinnable objects:

```text
files
symbols
diff hunks
findings
tests
commits
commands
notes
```

Example:

```text
INVESTIGATION

[authenticateUser]
[JWT validation diff]
[failed login test]
[external DB call]
[note: compare previous implementation]
```

The board is session-specific by default.

---

# 33. Time Machine

Aegis should provide structured navigation through repository history.

Select a commit and inspect:

* code,
* symbols,
* architecture,
* dependencies,
* Git state,
* known tests,
* historical Aegis data if available.

Example:

```text
Compare architecture:
HEAD
vs
4 weeks ago
```

---

# 34. Change Timeline

Aegis should maintain a chronological view.

Example:

```text
09:41 Codex task started
09:42 parser.cpp changed
09:42 tests failed
09:43 revision requested
09:44 parser.cpp changed
09:44 tests passed
09:45 dependency added
09:46 dependency removed
09:47 task completed
```

This becomes especially useful during long agent sessions.

---

# 35. Manual Editing

Aegis should not dogmatically prevent humans from editing code.

A direct edit mode should exist.

Possible command:

```text
Ctrl+E
```

Behavior:

* open selected file in configured editor, or
* use Aegis's minimal native editor.

Aegis records these as:

```text
Human change
```

Agent-first does not mean agent-only.

---

# 36. External Editor Integration

Users may configure:

```text
nvim
vim
helix
sublime_text
code
zed
```

Aegis may open:

```bash
$EDITOR file:line
```

This preserves workflows for users who already have a preferred editor.

---

# 37. Snapshots

Before agent operations, Aegis can create lightweight restore points.

A snapshot records:

```text
repository state
untracked files
modified files
Git base
```

Possible implementation mechanisms:

* Git stash-like data,
* patches,
* temporary worktrees,
* Aegis snapshot storage.

The user can instantly restore:

```text
Restore pre-task snapshot
```

---

# 38. Approval Gates

Sensitive projects may define action policies.

Examples:

```text
Agent may modify tests automatically.

Agent requires approval before modifying:
contracts/core/**
infra/prod/**
.github/workflows/**

Agent cannot:
git push
delete branches
modify .env
```

Policies should be repository-local and optional.

---

# 39. Agent Permissions

Aegis may eventually control:

```text
Filesystem
Network
Shell commands
Environment variables
Git operations
Process spawning
```

Example:

```text
AGENT PERMISSIONS

Filesystem
✓ repository
✗ ~/.ssh
✗ parent directories

Network
✓ allowed

Environment
✓ selected variables only

Git push
✗ blocked
```

---

# 40. Sandboxed Execution

Future Aegis versions may optionally run agents inside isolation mechanisms.

Platform-dependent possibilities:

```text
containers
Linux namespaces
Windows sandboxing
macOS sandbox profiles
```

Sandboxing is not required for the first MVP.

The architecture should avoid preventing it later.

---

# 41. Paranoia Mode

Aegis should inherit the philosophy of Jim's Paranoia Mode.

When enabled:

```text
No session history
No recent-project history
No persisted prompts
No telemetry
No analytics
Minimal caches
No cloud Aegis services
```

The status strip should make the mode obvious:

```text
☣ PARANOIA
```

Aegis itself should have no telemetry by default regardless.

---

# 42. Search

Aegis should provide fast repository search.

Commands:

```text
Ctrl+P
Quick file open

Ctrl+Shift+F
Repository text search

Ctrl+Shift+S
Symbol search
```

Search should remain available while the agent session stays visible.

---

# 43. LSP Integration

Aegis should operate as an LSP client.

Initial features:

```text
definitions
references
hover
diagnostics
document symbols
workspace symbols
```

Aegis should automatically detect common language servers but continue functioning without them.

Possible servers:

```text
clangd
rust-analyzer
gopls
pyright
typescript-language-server
solidity language server
```

---

# 44. Symbol Inspector

Selecting a symbol should expose:

```text
Definition
References
Callers
Callees
Tests
History
Recent changes
Agent provenance
Diagnostics
Findings
```

This becomes one of the central investigation tools.

---

# 45. Command Palette

Aegis should provide a universal action launcher.

Shortcut:

```text
Ctrl+Shift+P
```

Possible actions:

```text
Review current changes
Run verification
Open blast radius
Create snapshot
Switch agent
Open symbol inspector
Open architecture
Enable paranoia
Run security scan
Open investigation board
```

---

# 46. Shell Command Tracking

Because agents frequently interact with the shell, commands matter.

Aegis should record commands executed within its managed session.

Examples:

```text
cmake --build build
ctest
git diff
rg parseToken
forge test
cargo check
```

The trace should include:

```text
exit code
duration
captured output
```

where practical.

---

# 47. Process Tree

Advanced inspection should show processes spawned during a task.

Example:

```text
Codex
└── bash
    ├── rg
    ├── cmake
    │   └── ninja
    └── ctest
```

This may become useful for debugging agent behavior and identifying unexpected commands.

---

# 48. Resource Visibility

Optional HUD-style metrics:

```text
CPU
Memory
agent runtime
command duration
files touched
```

This should remain subtle.

No permanent cyberpunk dashboard occupying half the screen.

---

# 49. Session Handoff

Aegis should eventually allow switching agents during one task.

Example:

```text
Codex produced implementation.
Send current capsule to Claude for review.
```

The second agent receives structured context:

```text
original request
diff
test results
selected files
findings
```

rather than the entire raw session transcript.

---

# 50. Critic Mode

A useful future workflow:

```text
Implementer:
Codex

Critic:
Claude
```

Claude receives the completed changeset and is explicitly asked to identify issues.

Aegis keeps implementation and critique separate.

The human still decides.

---

# 51. Compare Mode

Aegis should eventually compare:

```text
current implementation
previous implementation
agent proposal A
agent proposal B
```

across dimensions such as:

```text
diff size
dependencies
verification results
API changes
findings
performance tests
```

Aegis should present evidence rather than automatically declare a winner.

---

# 52. Session Memory

Aegis may store project-local knowledge.

Examples:

```text
common verification commands
repository architecture
important paths
historical agent sessions
known fragile tests
user notes
```

This information should remain inspectable and removable.

---

# 53. Project Configuration

Optional configuration file:

```text
.aegis/config.toml
```

Possible configuration:

```toml
default_agent = "codex"

[verification]
fast = ["cmake --build build", "ctest"]

[permissions]
deny_paths = [".env", ".git/hooks"]

[review]
dependency_changes = "always"
public_api_changes = "always"
```

Avoid requiring configuration to start.

---

# 54. CLI Design

Core commands:

```bash
aegis .
aegis /path/to/repo

aegis . --agent codex
aegis . --agent claude

aegis review
aegis inspect
aegis status
aegis trace
aegis verify
aegis snapshot
```

Potential short forms can be added later.

---

# 55. Startup Performance

Targets:

```text
shell visible: <300 ms
repository status: <500 ms typical
agent start: effectively immediate
```

Heavy analysis should continue asynchronously after startup.

Aegis must never delay opening the agent because architecture indexing is still running.

---

# 56. Large Repository Strategy

Initial startup should perform only lightweight work:

```text
repository root
Git status
file enumeration
configuration detection
agent launch
```

Then progressively compute:

```text
symbols
references
architecture
risk models
security analysis
```

Do not parse the entire repository before becoming usable.

---

# 57. Persistence

Local database:

```text
SQLite
```

Potential data:

```text
sessions
capsules
traces
verification results
provenance
findings
project metadata
```

Raw source code should not be duplicated unnecessarily.

Paranoia Mode disables persistence.

---

# 58. MVP Scope

The first usable Aegis should include only the foundation.

## MVP requirements

1. `aegis .`
2. agent selector
3. run Codex/Claude/OpenCode/shell under Aegis
4. managed terminal session
5. repository file watching
6. Git status tracking
7. status strip
8. task/change session tracking
9. changed-file summary
10. unified diff review
11. side-by-side diff review
12. command trace where feasible
13. verification commands
14. test/build result display
15. snapshots or isolated worktree support
16. revision prompts tied to changeset
17. Paranoia Mode
18. external editor integration

That is sufficient to validate the core product.

---

# 59. Phase 2

After the core workflow feels excellent:

```text
LSP
symbol inspector
semantic diffs
blast radius
provenance
security findings
dependency monitoring
test-impact estimation
```

---

# 60. Phase 3

Advanced investigation:

```text
architecture graphs
risk heatmaps
investigation board
change timeline
time machine
multi-agent proposals
critic mode
```

---

# 61. Phase 4

Specialized lenses:

```text
Solidity
binary analysis
systems debugging
formal verification
```

Aegis should become extensible enough that new lenses can be added without changing the core agent workflow.

---

# 62. Jim Migration Strategy

Do not migrate Jim wholesale.

Jim v1 should remain intact.

Ideas worth selectively reincorporating include:

```text
Paranoia Mode
Solidity analysis
storage layout tooling
binary inspection
code graphs
keyboard-first navigation
strong visual identity
local-first philosophy
```

Avoid directly porting:

```text
old TextEditor architecture
manual syntax engine
large permanent toolbar/menu system
animation-heavy UI
story engine
editor-specific plumbing
```

Aegis should inherit Jim's spirit rather than its structure.

---

# 63. Explicit Non-Goals

Aegis should not initially become:

```text
VS Code replacement
full IDE
general-purpose text editor
extension marketplace
cloud development environment
project management tool
chat application
GitHub replacement
CI service
agent model provider
```

If a mature existing tool already solves something well, integrate it.

---

# 64. Product Design Rule

Before implementing any major feature, ask:

> Does this improve visibility, control, understanding, reversibility, or verification of software work?

If yes, it probably belongs.

If the feature merely duplicates conventional IDE functionality, it probably does not.

---

# 65. UX Rule

Aegis should never require more attention than the work itself.

The default experience must stay quiet.

Example:

```text
Agent working normally.
Status strip quietly updates.
No modal.
No dashboard.
No forced review.
```

The user expands Aegis when needed.

---

# 66. Product Personality

Aegis should feel:

```text
technical
dense
precise
fast
slightly intimidating
clean
serious
hacker-oriented
```

It should not feel:

```text
corporate SaaS
cute
chatbot-first
overanimated
neon for the sake of neon
```

Aegis may retain subtle cyberpunk influence, but information density and legibility come first.

---

# 67. Visual Direction

Primary theme:

```text
dark
near-black
cool grey
white text
cold blue/cyan accents
warning amber
error red
```

Animations should be:

```text
fast
minimal
purposeful
```

Examples:

* review panel expansion,
* changed-file highlight,
* finding appearance,
* verification completion.

No decorative animations during normal coding.

---

# 68. Key Shortcuts

Initial proposal:

```text
Ctrl+R
Review current change capsule

Ctrl+Shift+P
Command palette

Ctrl+Shift+F
Repository search

Ctrl+P
Quick open

Ctrl+Shift+O
Open deep inspection

Ctrl+Shift+V
Run verification

Ctrl+Shift+S
Create snapshot
```

Exact bindings should be revisited after prototype testing.

---

# 69. Definition of Success

Aegis succeeds when the user can spend an entire coding session inside:

```bash
aegis .
```

while still using their preferred coding agent.

They should rarely need to leave the Aegis session to answer questions such as:

```text
What did the agent change?
Why?
What commands did it run?
What broke?
What depends on this?
Did tests pass?
Did the public API change?
Did dependencies change?
Did it introduce something suspicious?
Can I revert this?
Can I ask it to revise just this part?
```

---

# 70. MVP Definition of Done

The first major Aegis milestone is complete when a user can:

1. run `aegis .`,
2. launch Codex, Claude Code, OpenCode, or a shell,
3. interact with that tool normally,
4. see repository changes appear live,
5. inspect changed files without leaving the session,
6. open a proper diff,
7. see commands and verification results,
8. run tests/build/lint from Aegis,
9. review the resulting evidence,
10. request a revision,
11. revert or discard the changes,
12. optionally run the task in an isolated worktree,
13. return immediately to the agent session.

At that point the core thesis has been proven.

---

# 71. Long-Term Vision

As coding agents improve, software engineering shifts from:

```text
human writes every line
```

toward:

```text
human specifies
machine implements
human investigates
human verifies
human decides
```

Aegis should become the environment for the final three steps.

The long-term product is therefore:

> **A control plane for human supervision of machine-produced software.**

The coding agent may change.

The models may change.

The interfaces may change.

The need to understand and control what those systems do should remain.

That is Aegis.

