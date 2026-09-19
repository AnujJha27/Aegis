# Aegis Frontend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the native Qt frontend described by `.frontend.md` while keeping the default workspace session-first, dense, calm, keyboard-accessible, and free of dashboard clutter.

**Architecture:** Keep the existing single-window Qt Widgets vertical slice and reuse its agent, diff, report, and session data. Add one small UI-theme module for shared visual tokens, then reshape `Window` into a compact header, full-width session surface, bottom status rail, and hidden review/deep-view drawer. Secondary actions remain in the existing command palette and new keyboard shortcuts.

**Tech Stack:** C++23, Qt6 Core/Widgets, existing Aegis libraries, CMake/CTest.

**Spec:** `.frontend.md`

## Global Constraints

- Use the native Qt Widgets implementation; do not add a web frontend or UI dependency.
- Default screen centers the agent session and keeps the review drawer hidden.
- Use the palette and keyboard shortcuts for secondary actions; avoid a twenty-action toolbar.
- Use the palette from `Ctrl+Shift+P`, review from `Ctrl+R`, and quick open from `Ctrl+P`.
- Keep colors restrained: dark layered surfaces, cold blue selection, muted green success, amber warning, muted red failure.
- Never stage or commit `.frontend.md`; add it to `.gitignore` first.
- Commit each completed task with only Aegis files and run the relevant checks before the next task.

## Review Focus

- A narrow window must keep the session usable and turn the review drawer into an overlay instead of shrinking the terminal to zero; test with a 760px-wide offscreen launch.
- Repeated status refreshes must not create overlapping processes or freeze clicks; test with the existing headless smoke run.
- Closing review must restore the session without losing the active terminal contents or selected file; test the shortcut path manually through the Qt event loop.
- Empty changes must show concise empty-state copy instead of blank panels; test the no-diff CLI/UI path.
- `.frontend.md` must be ignored before commits and absent from staged paths; verify with `git check-ignore` and `git diff --cached --name-only`.

### Task 1: Theme contract and tracking guard

**Files:**
- Create: `include/aegis/ui.h`
- Create: `src/ui.cpp`
- Create: `tests/ui_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `.gitignore`

**Interfaces:**
- Produces `aegis::ui::styleSheet()` for the application stylesheet and `aegis::ui::statusRail(const QString &, const QString &, const QString &, const QString &)` for compact rail text.

- [ ] **Step 1: Write the failing test**

  Add assertions that `styleSheet()` contains the required background, text, blue selection, and success colors, and that `statusRail("3", "+9", "-2", "IDLE")` produces the compact text `Δ 3 FILES    +9 −2    IDLE`.

- [ ] **Step 2: Run the focused test and verify it fails**

  Run `cmake -S . -B build && cmake --build build --target aegis_ui_test -j2`.
  Expected: configuration/build fails because the UI module and test target do not exist.

- [ ] **Step 3: Implement the theme module and ignore rule**

  Add the tokenized Qt stylesheet and status formatter. Add `/.frontend.md` to `.gitignore`. Add `aegis_ui` and `aegis_ui_test` to CMake.

- [ ] **Step 4: Run the focused test and full suite**

  Run `cmake --build build --target aegis_ui_test -j2 && ctest --test-dir build -R 'aegis_ui_test' --output-on-failure`.
  Expected: the focused test passes.
  Then run `ctest --test-dir build --output-on-failure`; expected: all tests pass.

- [ ] **Step 5: Verify the tracking guard and commit**

  Run `git check-ignore -v aegis/.frontend.md` and `git diff --cached --name-only` after staging only `.gitignore`, the UI module, CMake, and the test.
  Expected: the ignore command reports `aegis/.gitignore` and `.frontend.md` is absent from staged paths.
  Commit: `git add -- aegis/.gitignore aegis/CMakeLists.txt aegis/include/aegis/ui.h aegis/src/ui.cpp aegis/tests/ui_test.cpp && git commit -m "feat: add aegis frontend theme foundation"`.

### Task 2: Session-first window shell

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes `aegis::ui::styleSheet()` and `aegis::ui::statusRail()`.
- Produces a window with a thin header, full-width session terminal by default, compact prompt row, and bottom status rail.

- [ ] **Step 1: Add a shell smoke assertion**

  Extend `tests/ui_test.cpp` with a string-level assertion for the required shortcuts `Ctrl+R` and `Ctrl+P`; keep this test independent of display hardware.

- [ ] **Step 2: Run the focused test and verify it fails**

  Run `cmake --build build --target aegis_ui_test -j2 && ctest --test-dir build -R 'aegis_ui_test' --output-on-failure`.
  Expected: FAIL because the current main window does not expose the new shell contract.

- [ ] **Step 3: Reshape `Window`**

  Apply the stylesheet, replace the action-heavy toolbar with project/agent/session labels and only Review/Palette affordances, move the status label to the bottom rail, make the terminal the default full-width center, and keep the existing prompt row. Preserve all existing signal handlers and agent transport.

- [ ] **Step 4: Verify the shell**

  Run `cmake --build build -j2`, `ctest --test-dir build --output-on-failure`, and `QT_QPA_PLATFORM=offscreen timeout 5s ./build/aegis . --agent shell`.
  Expected: build/tests pass and the GUI remains alive until timeout with no crash.

- [ ] **Step 5: Commit**

  Commit only `src/main.cpp` and the updated UI test as `feat: make the session shell the default workspace`.

### Task 3: Review drawer and keyboard navigation

**Files:**
- Modify: `src/main.cpp`
- Modify: `tests/ui_test.cpp`

**Interfaces:**
- Produces `Ctrl+R` review toggle, `Escape` close/return behavior, and a right-side review drawer that reuses the existing diff/evidence widgets.

- [ ] **Step 1: Add failing interaction assertions**

  Test the review state formatter/helper for hidden, open, and closed states; assert that the default is hidden and that the selected file remains stable across a close/open cycle.

- [ ] **Step 2: Run focused test and verify failure**

  Run `ctest --test-dir build -R 'aegis_ui_test' --output-on-failure`.
  Expected: FAIL because the current UI has no drawer state.

- [ ] **Step 3: Implement the drawer**

  Hide the right pane initially, show it from Review/`Ctrl+R`, keep it 360–460px wide when space permits, switch to overlay behavior for narrow windows, add compact Summary/Changed files/Verification/Findings/Actions sections using existing widgets, and bind Escape to return to the session.

- [ ] **Step 4: Verify and commit**

  Run the focused test, full CTest, and the offscreen smoke command. Commit as `feat: add keyboard review drawer`.

### Task 4: Quick open, activity rendering, and calm deep views

**Files:**
- Modify: `src/main.cpp`
- Modify: `tests/ui_test.cpp`

**Interfaces:**
- Produces `Ctrl+P` quick open from changed files, compact agent activity labels, and existing evidence/symbol/architecture/timeline tabs styled as deep views rather than permanent panels.

- [ ] **Step 1: Add failing contract assertions**

  Assert that quick open includes changed files and that activity labels use `◆`/`├`/`└` markers without chat-bubble markup.

- [ ] **Step 2: Run focused test and verify failure**

  Run `ctest --test-dir build -R 'aegis_ui_test' --output-on-failure`.
  Expected: FAIL until the helper and UI path exist.

- [ ] **Step 3: Implement the minimum navigation surface**

  Add a filtered quick-open dialog, compact agent-start/activity lines, keep deep tabs available only inside review/investigation contexts, and preserve existing palette actions.

- [ ] **Step 4: Verify and commit**

  Run `cmake --build build -j2`, `ctest --test-dir build --output-on-failure`, and the offscreen smoke command. Commit as `feat: add quick open and deep-view navigation`.

### Task 5: Final acceptance review

**Files:**
- Modify: `.superpowers/sdd/plan/progress.md`
- Modify: `README.md` if keyboard commands or layout behavior need documentation.

- [ ] **Step 1: Run the full verification set**

  Run `cmake -S . -B build`, `cmake --build build -j2`, `ctest --test-dir build --output-on-failure`, `QT_QPA_PLATFORM=offscreen timeout 5s ./build/aegis . --agent shell`, and `git check-ignore -v aegis/.frontend.md`.

- [ ] **Step 2: Perform the final source review**

  Confirm the default layout contains no permanent file tree or action-heavy toolbar, the review drawer is hidden initially, the four shortcuts are wired, and `.frontend.md` is not staged.

- [ ] **Step 3: Record the result and commit documentation if changed**

  Append the verification result to the SDD ledger. If README changed, commit it separately as `docs: document aegis frontend navigation`; otherwise leave the documentation commit out.
