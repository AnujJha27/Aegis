# Aegis

A local-first control plane around coding agents.

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the GUI with `./build/aegis .` or choose an agent with `./build/aegis . --agent codex`.

Headless commands include:

```text
aegis status [path]
aegis review [path]
aegis analyze [path]
aegis lens [path]
aegis history [path]
aegis board [path]
aegis verify [path] -- command args...
aegis worktree [path]
```

The GUI opens as a quiet session-first console. `Ctrl+R` opens the review drawer, `Ctrl+P` opens filtered Quick Open for changed files, `Ctrl+Shift+P` opens the command palette, and `Escape` returns to the session. Review includes a readable line-numbered unified diff, evidence, symbols, architecture, risk, timeline, board, specialized lenses, LSP probing, worktrees, critic mode, proposal comparison, and agent handoff.
