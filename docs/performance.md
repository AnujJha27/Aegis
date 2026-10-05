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

## Review latency changes — 2026-10-05

The current implementation avoids Git enumeration on tree expansion and quick-open, publishes an index root snapshot before full indexing, virtualizes code/tree/diff DOM rows, caches review reads, and refreshes repositories according to active event subscribers. Monaco remains an explicit advanced view. Mermaid and xterm are lazy features. No executable packing or SQLite durability changes were made.

### Reproduce

```sh
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/aegis_perf_bench --all-scales > after.log
python3 scripts/compare-perf.py before.log after.log
cd web
npm test
npm run build
cd ..
node scripts/bundle-sizes.mjs
node --expose-gc scripts/review-soak.mjs
node scripts/mermaid-smoke.mjs
```

The TypeScript algorithm smoke/soak scripts require Node 24. The benchmark retains event/verification/agent lifecycle, PTY and real loopback WebSocket workloads. Review fixtures contain approximately 1k, 10k and 100k mostly untracked files across directories, plus tracked content for Git comparisons. Startup measures fresh synchronous index construction; the daemon uses the same builder on a background worker. `root_available_us` measures publication, not browser paint or complete daemon startup. `indexed_tree_us` excludes Git status overlays. Forty varied quick-open queries establish p50/p95; literal search uses ten Git `grep -F --untracked --exclude-standard` samples. Cold and warm opens are single samples, warm immediately following cold. Large-file indexing traverses an 8 MiB synthetic buffer. Fixture creation/cleanup is outside review timings. These are Linux/WSL local filesystem smoke measurements, not robust hardware-independent guarantees.

Saved baseline and final metric records are in [performance-review-before.log](performance-review-before.log) and [performance-review-after.log](performance-review-after.log). The baseline ran alongside initial checks/build, so small differences in existing metrics should not be interpreted as causal improvements. Timing output never fails CI. The optional comparison script warns only about large changes (over 50%, above a noise floor); result/capacity/security assertions remain gated.

### Comparable existing measurements

| Measurement | Before | After |
| --- | ---: | ---: |
| 10k Git change enumeration | 61 ms | 51 ms |
| Existing root route, including Git overlay | 44 ms | 27 ms |
| Existing selected comparison | 22 ms | 22 ms |
| Forced refresh p50 / p95 | 61 / 62 ms | 50 / 51 ms |
| SQLite latest 20 events p50 / p95 | 64 / 111 µs | 49 / 103 µs |
| PTY 128 KiB | 17 ms, 2 chunks | 9 ms, 2 chunks |
| Peak RSS before extra scale fixtures | 21,264 KiB | 23,032 KiB |

Quick-open and cached reopening did not have equivalent baseline probes; their final measurements below are not presented as speedup ratios.

### New review measurements

| Measurement | ~1k files | ~10k files | ~100k files |
| --- | ---: | ---: | ---: |
| First root snapshot | 8.46 ms | 19.66 ms | 152.88 ms |
| Complete index build | 29.97 ms | 158.37 ms | 1,930.46 ms |
| Indexed root listing | 0.039 ms | 0.090 ms | 0.561 ms |
| Quick-open p50 / p95 | 0.035 / 0.066 ms | 0.383 / 0.509 ms | 5.837 / 7.287 ms |
| Literal workspace search p50 / p95 | 80.51 / 83.98 ms | 680.39 / 701.04 ms | 5,356.52 / 6,677.31 ms |
| Cold worktree file open | 0.128 ms | 0.180 ms | 0.226 ms |
| Cached file reopen | 0.063 ms | 0.119 ms | 0.186 ms |
| Cold diff open | 22.955 ms | 22.225 ms | 29.185 ms |
| Cached diff reopen | 0.111 ms | 0.117 ms | 0.174 ms |

Backend 8 MiB line-offset traversal took 10.65 ms (104,858 offsets). The actual frontend algorithm indexed 8.4 million characters / 400,001 lines in 142.59 ms in Node, including source-keyed cache bookkeeping. Neither is a browser click-to-paint measurement. Full indexing is nonblocking for serving the UI, but Git root bootstrap itself still costs time, especially for tracked repositories. Until enumeration finishes, quick-open accepts input and exposes indexing progress rather than pretending results are complete.

Serial native scoring already meets the 100k design target here; a search worker pool would add synchronization without demonstrated benefit. No persistent `git cat-file --batch` child was added: cached reopen is already below one millisecond, while the remaining 23–29 ms cold comparisons warrant measuring a representative cold-blob session first. Git snapshot caching and ordinary filesystem reads remain in place.

### Production bundles

Sizes below are decimal kB. Baseline gzip comes from Vite; final exact gzip uses Node's default zlib settings, so gzip deltas are approximate. Run `scripts/bundle-sizes.mjs` for per-asset byte counts.

| Asset/path | Before raw / gzip | After raw / gzip |
| --- | ---: | ---: |
| Initial JavaScript entry payload | 584.54 / 162.01 kB | approximately 251.82 / 77.92 kB |
| Default lazy file viewer | 2,280.69 / 585.09 kB (Monaco) | 11.30 / 4.17 kB |
| Advanced Monaco chunk | included in old FileViewer | 2,277.92 / 578.85 kB |
| Terminal chunk | in initial payload | 334.06 / 83.71 kB |
| Markdown preview chunk | absent | 77.37 / 26.95 kB |
| Mermaid core chunk | absent | 94.89 / 32.39 kB |

Mermaid core is only part of Mermaid's payload: diagram grammars/layout engines and shared chunks load separately on demand. It is incorrect to call the core chunk the total diagram download. The build also retains Monaco editor/JSON worker assets for the fallback; these are not part of the default review path. Initial CSS is 35.57 / 8.30 kB raw/gzip (before 36.94 / 8.45 kB). It includes diagram controls, but no Mermaid JavaScript is in the initial static module graph. Markdown without Mermaid never imports Mermaid. Terminal rendering loads only for an interactive run; readiness reconnects output and replays persisted PTY history to prevent lazy-loading output loss.

### Boundedness and memory

| Structure | Bound |
| --- | --- |
| Repository index | 500,000 paths and 16 MiB Git enumeration output |
| Quick-open | 256-byte query; API 60 results; native maximum 100 |
| Content and comparison caches | independently 128 entries / 32 MiB accounted payload each |
| Object identity cache | 4,096 entries / 1 MiB |
| Frontend parsed lines | 12 documents / 32 MiB source plus offsets |
| Restored scroll positions | 32 identities |
| Expanded/cached tree directories | 128; directory responses paged at 500 entries |
| Virtual row DOM | viewport plus 12 rows overscan per side; no full-document row mount |
| Diff representation | compressed hunks, expanded only for visible rows |
| Mermaid SVG cache | 100 entries / 8 MiB; source 50k characters, edges 1k |
| Browser performance samples | 200 per each of five fixed metrics |
| Terminal render/replay identities | 3,000 scrollback lines / 4,096 IDs |
| Pending terminal input | 128 chunks / 16,384 characters |
| Event subscriber queues | 512 events / 4 MiB, then resync |

Cache byte accounting is a payload ceiling, not allocator/process RSS. Watcher maps scale with the bounded Git status output rather than every historical file. Builder scratch and diff workers are released after use; only one index build and one diagram render run at a time. Immutable index readers and cache readers do not hold locks during Git/file I/O. Git status remains a separate overlay. Worktree cache identities include no-follow validated metadata, and changed files cannot reuse immutable blob identities.

Three backend review soak passes each open/search/compare 200 files after the 100k fixture: both review caches reached 128 entries and stayed there, and RSS stayed at 94,964 KiB across all three passes. Whole-process final peak also includes fixtures and index snapshots and is not comparable with the old 10k-only peak. All benchmark-owned threads returned to the initial count and temporary fixtures were removed.

The Node algorithm soak switches 300 documents and computes 30 diffs per pass. Post-GC heap stayed approximately 17.37–17.38 MiB over four passes. RSS rose from 127.75 to 204.13 MiB: this does **not** establish a process/browser RSS plateau. A real browser long-session profile is still required. Newline-heavy 100k-line diffs are tested for compressed representation as well as bounded DOM.

### Browser instrumentation and functional checks

Add `?perf` to the local UI URL (also enabled during development), then inspect `window.aegisReviewPerformance()`. It reports bounded p50/p95 samples for file click→first rows, diff click→first content, diff click→first computed hunk, quick-open→visible response, and scroll RAF scheduling. Click timing includes lazy loading and network requests where available. Scroll samples measure scheduled rendering latency, not full GPU paint cost or proof of 60 fps.

Final checks: 9/9 backend suites, 17 frontend files / 55 tests, five rerun Mermaid security checks, production TypeScript/build and the real Mermaid smoke check passed. Backend checks cover ignored paths/directories, creation/deletion/rebuilds, concurrent index reads, 10k ranking/result bounds, cache eviction, same-sized worktree edits, symlink rejection, repository versions and watcher metadata/worktree invalidation. Existing API/e2e suites continue exercising persistence, Git ranges, agent/PTY and reconnect/resync behavior. Frontend checks cover bounded visible rows, scrolling/find, compressed diff layout, cancellation of stale file/quick-open requests, affected-path refresh and tree pagination; Mermaid checks cover lazy loading, isolated invalid diagrams, cache reuse/eviction, theme changes, source copy, fullscreen Escape, stale rendering and malicious configuration/HTML/SVG/CSS. The real pinned Mermaid smoke script renders flowchart and sequence diagrams with jsdom geometry stubs and sanitizes their SVGs; it is a correctness check, not a browser benchmark.

### Remaining work driven by measurements

Literal workspace search still invokes Git and reads disk; at 100k it is the largest measured latency. First profile a realistic search corpus, then consider bounded cancellable search workers or an incremental text index only if the memory tradeoff is justified. Profile startup on a large fully tracked repository before choosing native filesystem notifications or cheaper root bootstrap. Consider a persistent Git object reader only for a workload dominated by cold immutable blob reads.

The lightweight tokenizer covers common lexical tokens, not Monaco-level semantic or multiline grammar parity. Patience diff alignment is bounded and useful but can produce non-minimal replacements in unmatched repeated blocks; the advanced viewer remains available. Virtual selection supports visible native text, whole-file copy and shift line-range copy; it does not promise editor-grade rectangular selection. Exact changed-line gutters require additional source mapping and are not populated from file-level Git badges.

Not measured: real browser paint/scroll FPS, screen-reader navigation across virtual rows, theme appearance across every Mermaid grammar, idle daemon CPU, hardware variability, or a browser memory plateau. No GUI automation runtime was available and Sol was not consulted. These limitations should guide follow-up validation, not be hidden behind synthetic timings.
