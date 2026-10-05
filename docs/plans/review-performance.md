# Review latency implementation plan

The user's performance specification is the approved scope. Preserve local-only security, Git ranges, agent/session lifecycle, size limits and existing graph views.

1. Measure the existing backend tests, synthetic benchmark and production bundles. Add an immutable RepositoryIndex owned by Files, built in the background in App. Publish root before metadata, keep flat candidates and directory children, expose progress/version, and provide bounded native fuzzy quick-open. Test ignored paths, rebuilds, ranking and concurrent reads.
2. Replace the default Monaco rendering with virtual code/diff rows. Keep Monaco behind an explicit advanced viewer. Index lines once, highlight visible rows, restore bounded recent scroll positions, support find/go-to/copy and both diff layouts. Cancel all file loads, including explicit large loads. Test row bounds and stale loads.
3. Add byte/entry-bounded review caches. Preserve safe worktree opens and validate descriptor metadata before cache hits. Move Git snapshot subprocess work outside its publication lock. Watch actual Git metadata including linked worktrees, adapt worktree scans to active clients, publish compact version events, and refresh only affected views.
4. Expand the existing benchmark with index, quick-open distributions, cold/warm opens and line indexing; add browser bounded timing samples and reproducible bundle reports. Report measurements without CI timing gates.
5. Add Markdown preview with escaped/sanitized content and independent lazy Mermaid diagrams, bounded SVG cache, cooperative rendering, theme awareness and zoom/pan/fullscreen/source controls. Enforce strict config and sanitize generated SVG. Test malformed/security/staleness/cache interactions and inspect lazy output.

Verification: existing backend and frontend suites plus focused new checks; production typecheck/build; performance run on same host. Commit each subsystem. Persistent cat-file is evaluated after cache measurements; implement only with evidence of benefit. Browser timing/soak measurements require a real GUI host and must not be fabricated.
