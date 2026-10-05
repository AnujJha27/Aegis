# Review performance workflow

Phase: all five implementation phases complete and verified.
Next action: review the perf/review-latency branch; profile browser paint/RSS and literal search before further optimization.
Decisions: retain C++/React and existing Git/security semantics; immutable background repository snapshots, bounded caches, virtual review by default and explicit Monaco fallback. Serial quick-open meets measured 100k targets; persistent cat-file is deferred until cold-object workloads justify it. Compressed patience diff hunks keep newline-heavy documents bounded; advanced Monaco remains available for detailed alignment.
Blockers: none. Browser tooling unavailable; Sol was not consulted. Local socket tests use reviewed sandbox escalation. Linux Node 24 replaces the environment's Windows npm wrapper.
Checks: baseline backend 7/7 and frontend 41 tests; new backend suite 9/9 and focused root-bootstrap checks; frontend 55 tests passed, followed by five final Mermaid security checks. All-scale benchmark passed: quick-open 100k p50/p95 5.84/7.29 ms, cached diff 0.17 ms; root snapshot 153 ms / full index 1.93 s. Backend three-pass RSS flat at 94,964 KiB; Node heap flat ~17.38 MiB, RSS does not plateau. Production JS about 252 kB / 78 kB gzip versus 585 kB / 162 kB baseline.
Known limits: disk-based literal search ~5.4 s at 100k; full tracked repository startup and GUI paint/FPS/RSS need profiling. Simple tokenization and patience alignment retain explicit advanced fallback. Original GraphView and local-first agent/persistence architecture preserved.
