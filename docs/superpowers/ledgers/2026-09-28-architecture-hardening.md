# SDD ledger — plan: docs/superpowers/plans/2026-09-28-architecture-hardening.md

Setup: direct inline implementation authorized by the user's explicit request; use `.aegis-git` (the standalone Aegis repository), not the parent workspace Git repository. Sol/browser integration unavailable. Preexisting build artifacts and parent-workspace changes are out of scope.

Ruling: run state and turn state are distinct; interrupt leaves a resumable run `running` while a `turn.interrupted` event records the interrupted request. A busy run rejects another semantic prompt with HTTP 409 rather than queueing.

Progress: implemented the schema-v1 migration, authoritative run lifecycle/provider-session persistence, persisted verification history, bounded handoff evidence, semantic prompt events, resumable/cancellable Codex turns, PTY process exit classification/reaping, manager/API/server ownership fixes, route split, lightweight frontend hooks, and the Qt-free default launcher. Git discovery is isolated to the opened repo, including this checkout's `.aegis-git` metadata.

Verification: default non-Qt build passed; optional `AEGIS_BUILD_LEGACY=ON` configure and `aegis_legacy` build passed; CTest passed 4/4 with loopback permission; frontend typecheck/build passed; `aegis <repo>` smoke test returned a loopback URL and `/api/repository` returned the Aegis repository metadata. Live inspection URL: http://127.0.0.1:42227/.

Next: save the verified implementation as a local milestone commit. No push was requested in this task.
