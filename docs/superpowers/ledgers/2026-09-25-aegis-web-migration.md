# SDD ledger — plan: docs/superpowers/plans/2026-09-25-aegis-web-migration.md

Setup: repository-local ledger used because the skill workspace helper targets the read-only parent mount.
Pre-flight: existing Qt modules remain the legacy surface; new daemon modules use standard C++ types and are linked separately.
Task 1: complete — RED: daemon_store_test target missing; GREEN: SQLite store and EventHub implemented; verification: cmake --build build -j2 && ctest --test-dir build --output-on-failure → 10/10 passed.
Task 2: complete — RED: daemon_api_test target missing; GREEN: loopback Beast HTTP routes and WebSocket event endpoint implemented; verification: cmake --build /tmp/aegis-build -j2 && ctest --test-dir /tmp/aegis-build --output-on-failure → 11/11 passed with network permission.
Task 3: complete — RED: daemon_services_test target missing; GREEN: argv-safe process runner, Git/verification services, PTY adapters, Codex JSONL normalization, and agent routes implemented; verification: ctest --test-dir /tmp/aegis-build --output-on-failure → 12/12 passed.
Ruling: keep one focused daemon services integration test instead of adding helper-level tests — the user explicitly rejected test clutter, and process/Git/verification/adapter behavior is covered through the shared path.
Task 4: complete — standalone `aegis_daemon` owns lifecycle and loopback Beast server serves the frontend root with traversal rejection; reused `daemon_api_test` for static serving instead of adding a second app test. Verification: daemon target and API integration test build/run; full legacy suite remains available, with the pre-existing terminal test requiring its normal PTY environment.
Ruling: no extra daemon app test binary — startup is covered by the executable smoke check and API integration path.
