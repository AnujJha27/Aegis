import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { api, type HandoffContext } from "../../app/api";
import { HandoffPanel } from "./HandoffPanel";

afterEach(() => { cleanup(); vi.restoreAllMocks(); });

describe("handoff verification evidence", () => {
  it("normalizes the persisted verification returned by the daemon", async () => {
    vi.stubGlobal("fetch", vi.fn().mockResolvedValue({ ok: true, status: 200, json: async () => ({
      task_id: "task-1", prompt: "Fix the failing check", recent_events: [], diff: "", changed_files: [],
      changed_files_truncated: false, findings: [], verification: {
        id: "verify-1", task_id: "task-1", run_id: null, command: ["ctest"], exit_code: 1,
        output: "1 test failed", started_at: 10, finished_at: 12,
      },
    }) }));

    const context = await api.handoff("task-1");

    expect(context.verification?.exitCode).toBe(1);
    expect(context.verification?.taskId).toBe("task-1");
  });

  it("shows and copies the latest persisted verification", async () => {
    const writeText = vi.fn().mockResolvedValue(undefined);
    Object.defineProperty(navigator, "clipboard", { configurable: true, value: { writeText } });
    const context: HandoffContext = {
      task_id: "task-1", prompt: "Fix the failing check", recent_events: [], diff: "", changed_files: [],
      changed_files_truncated: false, findings: [],
      verification: { id: "verify-1", taskId: "task-1", runId: null, command: ["ctest", "--output-on-failure"], exitCode: 1, output: "1 test failed", startedAt: 10, finishedAt: 12 },
    };
    render(<HandoffPanel context={context} />);

    expect(screen.getByText("Failed (exit 1)")).toBeTruthy();
    expect(document.querySelector(".handoff-grid pre")?.textContent).toContain("1 test failed");
    fireEvent.click(screen.getByRole("button", { name: "Copy context" }));
    expect(writeText).toHaveBeenCalledWith(expect.stringContaining("1 test failed"));
  });
});

describe("handoff verification freshness", () => {
  const baseVerification = { id: "verify-1", taskId: "task-1", runId: null, command: ["ctest"], exitCode: 0, output: "all passed", startedAt: 10, finishedAt: 12 };

  function contextWith(freshness?: string): HandoffContext {
    return {
      task_id: "task-1", prompt: "Fix the failing check", recent_events: [], diff: "", changed_files: [],
      changed_files_truncated: false, findings: [],
      verification: { ...baseVerification, ...(freshness ? { freshness } : {}) } as any,
    };
  }

  beforeEach(() => {
    Object.defineProperty(navigator, "clipboard", { configurable: true, value: { writeText: vi.fn().mockResolvedValue(undefined) } });
  });

  async function copyContext(freshness?: string): Promise<string> {
    render(<HandoffPanel context={contextWith(freshness)} />);
    fireEvent.click(screen.getByRole("button", { name: "Copy context" }));
    const writeText = (navigator.clipboard as any).writeText;
    return writeText.mock.calls[0][0] as string;
  }

  it("labels a stale historical check in the copied context", async () => {
    const text = await copyContext("stale");
    expect(text).toMatch(/stale/i);
    expect(text).not.toContain("Status: Passed");
  });

  it("labels an unknown-freshness historical check in the copied context", async () => {
    const text = await copyContext("unknown");
    expect(text).toMatch(/unknown/i);
    expect(text).not.toContain("Status: Passed");
  });
});
