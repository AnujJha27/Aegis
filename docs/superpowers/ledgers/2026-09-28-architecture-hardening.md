# SDD ledger — plan: docs/superpowers/plans/2026-09-28-architecture-hardening.md

Setup: direct inline implementation authorized by the user's explicit request; use `.aegis-git` (the standalone Aegis repository), not the parent workspace Git repository. Sol/browser integration unavailable. Preexisting build artifacts and parent-workspace changes are out of scope.

Ruling: run state and turn state are distinct; interrupt leaves a resumable run `running` while a `turn.interrupted` event records the interrupted request. A busy run rejects another semantic prompt with HTTP 409 rather than queueing.
