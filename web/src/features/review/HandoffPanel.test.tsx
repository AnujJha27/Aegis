import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
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
