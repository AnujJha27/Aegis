import { describe, expect, it } from "vitest";
import type { AgentEvent, AgentRun, HandoffContext, ReviewFinding, VerificationRun } from "../../app/api";
import { formatHandoff } from "./handoffText";

const events: AgentEvent[] = [
  { id: "e-1", taskId: "t-1", runId: "run-42", type: "agent.message", agent: "codex", content: "Edited src/auth.ts · 設定完了", timestamp: 1 },
];

const verification: VerificationRun = {
  id: "v-1",
  taskId: "t-1",
  runId: "run-42",
  command: ["pnpm", "test"],
  exitCode: 0,
  output: "all green ✓",
  startedAt: 1,
  finishedAt: 2,
  freshness: "stale",
};

const findings: ReviewFinding[] = [
  { id: "f-1", task_id: "t-1", run_id: "run-42", file_path: "src/auth.ts", start_line: 10, end_line: 12, message: "缺少空值检查 · missing null check", status: "open", created_at: 1, updated_at: 1 },
];

const taskContext = {
  goal: "Ship SSO · シングルサインオン",
  constraints: "Keine Breaking Changes",
  decisions: "JWT verwenden",
  attempts: "Middleware first",
  unfinished: "Refresh tokens",
  next_action: "续写失败用例",
};

const baseContext = (overrides: Partial<HandoffContext> = {}): HandoffContext => ({
  task_id: "t-1",
  prompt: "Refactor auth module · 認証モジュールのリファクタリング",
  recent_events: events,
  diff: "--- a/src/auth.ts\n+++ b/src/auth.ts\n@@ -1 +1 @@\n-old\n+новая строка\n",
  changed_files: ["src/auth.ts", "src/auth.test.ts"],
  changed_files_truncated: false,
  verification,
  findings,
  ...overrides,
});

const sourceRun: AgentRun = { id: "run-42", taskId: "t-1", agent: "codex", status: "completed", startedAt: 1, finishedAt: 2 };

describe("formatHandoff", () => {
  it("includes the six context fields, source run, changes, findings, and verification details", () => {
    const text = formatHandoff({ ...baseContext(), context: taskContext } as HandoffContext, sourceRun);
    expect(text).toContain("Refactor auth module · 認証モジュールのリファクタリング");
    expect(text).toContain("Ship SSO · シングルサインオン");
    expect(text).toContain("Keine Breaking Changes");
    expect(text).toContain("JWT verwenden");
    expect(text).toContain("Middleware first");
    expect(text).toContain("Refresh tokens");
    expect(text).toContain("续写失败用例");
    expect(text).toContain("run-42");
    expect(text).toContain("src/auth.test.ts");
    expect(text).toContain("open src/auth.ts:10-12");
    expect(text).toContain("pnpm test");
    expect(text).toContain("exit 0");
    expect(text).toContain("Stale");
    expect(text).not.toContain("Passed");
  });

  it("never claims Passed for stale, unknown, or missing verification", () => {
    const stale = formatHandoff(baseContext(), sourceRun);
    expect(stale).toContain("Stale");
    expect(stale).not.toContain("Passed");
    const unknown = formatHandoff(baseContext({ verification: { ...verification, freshness: "unknown" } }), sourceRun);
    expect(unknown).not.toContain("Passed");
    expect(unknown).toContain("Freshness unknown");
    const missing = formatHandoff(baseContext({ verification: null }), sourceRun);
    expect(missing).toContain("No verification recorded");
    expect(missing).not.toContain("Passed");
  });

  it("stays within the 64KiB byte bound with a visible truncation marker while keeping task and next action", () => {
    const oversized = {
      ...taskContext,
      goal: ("Ω".repeat(500) + "·セクション·").repeat(60),
    };
    const text = formatHandoff({ ...baseContext(), context: oversized } as HandoffContext, sourceRun);
    expect(new TextEncoder().encode(text).length).toBeLessThanOrEqual(65536);
    expect(text).toContain("Refactor auth module");
    expect(text).toContain("续写失败用例");
    expect(text).toMatch(/truncat/i);
  });

  it("honors the 64KiB byte cap for giant multilingual changed files and diff", () => {
    const giant = Array.from({ length: 4000 }, (_, i) => `путь/файл-${i}-設定-Ω`.repeat(20));
    const text = formatHandoff(
      { ...baseContext(), changed_files: giant, changed_files_truncated: true, diff: "+ новая строка • 境界 •\n".repeat(20000), context: taskContext } as HandoffContext,
      sourceRun,
    );
    expect(new TextEncoder().encode(text).length).toBeLessThanOrEqual(65536);
    expect(text).toMatch(/truncat/i);
    expect(text).toContain("续写失败用例");
  });

  it("keeps the baseline handoff sections unchanged", () => {
    const text = formatHandoff(baseContext(), sourceRun);
    expect(text).toContain("Task: Refactor auth module · 認証モジュールのリファクタリング");
    expect(text).toContain("Changed files:");
    expect(text).toContain("Latest verification:");
    expect(text).toContain("Review findings:");
    expect(text).toContain("Diff:");
    expect(text).toContain("--- a/src/auth.ts");
    expect(text).toContain("новая строка");
  });
});
