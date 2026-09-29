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
