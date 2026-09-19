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

The GUI exposes the MVP review flow plus evidence, symbols, architecture, risk, timeline, board, specialized lenses, LSP probing, worktrees, critic mode, proposal comparison, and agent handoff from the toolbar or `Ctrl+Shift+P`.
