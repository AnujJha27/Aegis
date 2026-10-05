# Review performance workflow

Phase: baseline and repository index.
Next action: finish baseline checks, implement RepositoryIndex and native quick-open.
Decisions: retain C++/React architecture and existing Git security; explicit advanced Monaco fallback; bounded review and Mermaid caches. Use .aegis-git for commits because .git belongs to the containing workspace. Browser tooling unavailable; Sol was not consulted.
Blockers: none. Sandbox excludes loopback sockets; test/benchmark runs use reviewed escalation. Windows npm wrapper replaced by installed Linux Node/npm.
Progress: inspected architecture, repository/files/Git, API, process lifecycle, event hub, frontend app/review and benchmark. No product edits yet.

Milestone: RepositoryIndex, native quick-open, bounded review caches and activity-aware watcher compile; index/repository checks pass. Virtual file/tree/diff views pass 22 focused frontend checks. Added strict lazy Mermaid preview and tests; verification in progress.
Decisions: cached diagrams are namespaced per instance; configuration and external image/CSS directives fail safely with preserved source. xterm loads only for interactive runs, and terminal readiness requests persisted replay. Persistent cat-file remains under evaluation against cold/warm cache measurements.
Next action: complete backend and Mermaid checks, commit subsystems, run all-scale benchmark and production bundle inspection.

Verification milestone: full backend suite passed 9/9; frontend suite passed 53 tests before final compressed-hunk checks. Real pinned Mermaid flowchart and sequence rendering passes with jsdom layout stubs; this is not a browser latency test. Production initial JS fell from 584.54 kB to approximately 252 kB; final bundle and all-scale benchmark remain to record.
Ruling: use compressed diff hunks rather than one row object per line to bound newline-heavy documents. Keep simple patience alignment; repeated unmatched blocks can be non-minimal, with Monaco available for detailed comparison.
