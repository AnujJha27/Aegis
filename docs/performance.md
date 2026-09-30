# Performance baseline

Run `cmake --build build --target aegis_perf_bench -j2 && ./build/aegis_perf_bench` on the same machine and filesystem to compare results. It seeds the event fixture in one SQLite transaction (so fixture setup does not measure Aegis's per-event append path), times 40 latest-20 queries, emits 128 KiB through a synthetic PTY, and creates 10,000 untracked files in a temporary Git repository. The benchmark removes its temporary files on exit.

One Release run in the Linux/WSL development environment on 2026-09-29 produced:

| Measurement | Result |
| --- | ---: |
| 100,000-event fixture seed | 345 ms |
| Latest 20 events, p50 / p95 | 49 / 94 μs |
| SQLite database after fixture | 15,912,960 bytes |
| 128 KiB PTY output | 2 chunks, 8 ms |
| 10,000-file Git status/change listing | 60 ms |
| Lazy root listing (100 directories) | 50 ms |
| Selected untracked-file comparison | 19 ms |
| Benchmark process peak RSS | 14,008 KiB |

These are one-run smoke measurements, not statistically robust release benchmarks. The prior PTY reader read at most 8 KiB per emitted event, so the same 128 KiB burst required at least 16 output events; the current 64 KiB batch ceiling emitted 2 in this run. EventHub is hard-bounded to 512 events and 4 MiB per subscriber. PTY storage prunes to the most recent 128 chunks periodically and at run completion, checking every 32 inserted events; during a burst this is bounded to fewer than 160 chunks (under 10 MiB at 64 KiB/chunk) per active run.

The read-only Monaco bundle keeps only the editor and JSON workers. TypeScript, CSS, and HTML language-service workers were unnecessary for inspection-only rendering; compared with the prior build, their standalone worker assets (about 7.66 MB raw) are no longer emitted. The lazy FileViewer renderer remains about 2.28 MB raw / 585 KB gzip.

The shared Git snapshot reduces `/api/git/status` work from the prior duplicated pattern (status/numstat for both `state()` and `changes()`, plus independent current-branch and clean checks) to one status scan, one numstat scan, and one branch-list command. Calls within 250 ms reuse the versioned snapshot; explicit refresh and Aegis mutations invalidate it. The HTTP accept loop's idle poll changed from 5 ms to 250 ms (about 200 to 4 poll wakeups per second by interval, not a measured CPU benchmark).

Not measured here: daemon idle RSS/CPU with a browser attached, slow-browser socket buffering, browser frame latency, long-session memory plateau, or before/after sanitizer memory. Those require a stable GUI-capable host and are not represented by synthetic fixture numbers above. No WAL/synchronous durability change or asynchronous SQLite writer was made; semantic writes remain synchronous and durable before publication.

## Synthetic soak run

On 2026-09-30, the expanded `aegis_perf_bench` ran 50 task/run/event cycles with verification evidence and handoff retrieval, 8 shell-agent interrupt/terminate cycles, a 128 KiB PTY burst, a 10,000-file repository with 10 forced refreshes, and two EventHub subscribers (one deliberately left unread until it hit the configured queue bound). It also seeded 100,000 events transactionally for the existing history-query measurement. This is a repeatable component soak, not a whole-application performance claim.

| Measurement | Result |
| --- | ---: |
| CPU time | 3,748 ms |
| Peak / post-cleanup RSS | 16,104 / 16,080 KiB |
| SQLite size after cycles | 19,656,704 bytes |
| Latest 20 events, p50 / p95 | 51 / 113 μs |
| Verification request, p50 / p95 | 15,701 / 20,109 μs |
| Handoff retrieval, p50 / p95 | 500 / 761 μs |
| EventHub slow subscriber high-water | 502 events / 4,192,533 bytes; then `stream.resync_required` |
| PTY output | 131,072 bytes in 2 chunks / 9 ms |
| Agent interrupt/terminate cycles, p50 / p95 | 389 / 395 ms |
| 10,000-file status / root listing / selected comparison | 55 / 45 / 21 ms |
| Forced repository refresh, 10 samples p50 / p95 | 52 / 54 ms |

The daemon end-to-end test uses loopback WebSockets, reconnects the PTY output stream, and confirms persisted replay. The soak benchmark also floods two live event WebSockets: a fast reader receives all 1,200 8 KiB events while a delayed reader recovers through `stream.resync_required`. A real browser automation runtime is not available, so this workload does not measure browser reconnects or establish whole-application performance. RSS after cleanup stayed near peak in this short process run; this alone does not establish a leak or a long-session plateau.
