import { act, render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { api } from "./api";
import { App } from "./App";

vi.mock("../components/Layout", () => ({
  Layout: ({ connection, selectedTask }: { connection: string; selectedTask?: { id: string } }) => <div>
    <output aria-label="connection">{connection}</output>
    <output aria-label="selected task">{selectedTask?.id ?? "none"}</output>
  </div>,
}));
vi.mock("./useAgentWorkspace", () => ({
  useAgentWorkspace: () => ({ runs: [], setRuns: vi.fn(), selectedRunId: "", setSelectedRunId: vi.fn(), events: [], setEvents: vi.fn(), currentRun: undefined, currentEvents: [], currentRunEvents: [], runFinished: false, connection: "connected" }),
}));
vi.mock("./usePtySession", () => ({ usePtySession: () => ({ send: vi.fn(), resize: vi.fn() }) }));
vi.mock("./useReviewData", () => ({
  useReviewData: () => ({ verification: undefined, handoff: undefined, graph: undefined, provenance: undefined, refresh: vi.fn(), verify: vi.fn() }),
}));

describe("App startup recovery", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => { vi.useRealTimers(); vi.restoreAllMocks(); });

  it("retries bootstrap and restores tasks after a temporary daemon outage", async () => {
    vi.spyOn(api, "tasks").mockRejectedValueOnce(new Error("offline")).mockResolvedValue([{
      id: "task-1", prompt: "Fix it", repository: "/repo", status: "active", createdAt: 1,
    }]);
    vi.spyOn(api, "repository").mockResolvedValue({ path: "/repo", branch: "main" });
    vi.spyOn(api, "agents").mockResolvedValue([]);
    vi.spyOn(api, "gitStatus").mockResolvedValue({ repository: { path: "/repo" }, files: [], branches: ["main"], current_branch: "main", clean: true, agent_running: false });

    render(<App />);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(screen.getByLabelText("selected task").textContent).toBe("none");

    await act(async () => { await vi.advanceTimersByTimeAsync(500); });

    expect(screen.getByLabelText("selected task").textContent).toBe("task-1");
    expect(api.tasks).toHaveBeenCalledTimes(2);
  });
});
