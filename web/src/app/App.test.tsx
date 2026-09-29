import { act, cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { api } from "./api";
import { App } from "./App";

const workspaceMocks = vi.hoisted(() => ({ setRuns: vi.fn(), setSelectedRunId: vi.fn() }));

vi.mock("../components/Layout", () => ({
  Layout: ({ connection, selectedTask, error, onDismissError, onSelectTask, onLaunch }: { connection: string; selectedTask?: { id: string }; error: string; onDismissError?: () => void; onSelectTask?: (task: { id: string; prompt: string; repository: string; status: string; createdAt: number }) => void; onLaunch?: () => void }) => <div>
    <output aria-label="connection">{connection}</output>
    <output aria-label="selected task">{selectedTask?.id ?? "none"}</output>
    <button onClick={() => onSelectTask?.({ id: "task-2", prompt: "Other", repository: "/repo", status: "active", createdAt: 2 })}>Switch task</button>
    <button onClick={onLaunch}>Launch</button>
    {error && <div role="alert">{error}<button aria-label="Dismiss error" onClick={onDismissError}>Dismiss</button></div>}
  </div>,
}));
vi.mock("./useAgentWorkspace", () => ({
  useAgentWorkspace: () => ({ runs: [], setRuns: workspaceMocks.setRuns, selectedRunId: "", setSelectedRunId: workspaceMocks.setSelectedRunId, events: [], setEvents: vi.fn(), currentRun: undefined, currentEvents: [], currentRunEvents: [], runFinished: false, turnBusy: false, turnCompleted: false, turnInterrupted: false, connection: "connected" }),
}));
vi.mock("./usePtySession", () => ({ usePtySession: () => ({ send: vi.fn(), resize: vi.fn() }) }));
vi.mock("./useReviewData", () => ({
  useReviewData: () => ({ verification: undefined, handoff: undefined, graph: undefined, provenance: undefined, refresh: vi.fn(), verify: vi.fn() }),
}));

describe("App startup recovery", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => { cleanup(); vi.useRealTimers(); vi.restoreAllMocks(); });

  it("does not apply an old launch response after switching tasks", async () => {
    let resolveLaunch!: (run: { id: string; taskId: string; agent: string; status: string; startedAt: number; finishedAt: number }) => void;
    vi.spyOn(api, "tasks").mockResolvedValue([
      { id: "task-1", prompt: "Fix it", repository: "/repo", status: "active", createdAt: 1 },
      { id: "task-2", prompt: "Other", repository: "/repo", status: "active", createdAt: 2 },
    ]);
    vi.spyOn(api, "repository").mockResolvedValue({ path: "/repo", branch: "main" });
    vi.spyOn(api, "agents").mockResolvedValue([]);
    vi.spyOn(api, "gitStatus").mockResolvedValue({ repository: { path: "/repo" }, files: [], branches: ["main"], current_branch: "main", clean: true, agent_running: false });
    vi.spyOn(api, "launch").mockImplementation(() => new Promise((resolve) => { resolveLaunch = resolve; }));
    workspaceMocks.setRuns.mockClear();
    render(<App />);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(screen.getByLabelText("selected task").textContent).toBe("task-1");

    fireEvent.click(screen.getByRole("button", { name: "Launch" }));
    fireEvent.click(screen.getByRole("button", { name: "Switch task" }));
    await act(async () => { resolveLaunch({ id: "run-old", taskId: "task-1", agent: "shell", status: "running", startedAt: 1, finishedAt: 0 }); });

    expect(screen.getByLabelText("selected task").textContent).toBe("task-2");
    expect(workspaceMocks.setRuns).not.toHaveBeenCalled();
  });

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

  it("allows dismissing an error notice without reloading the page", async () => {
    vi.spyOn(api, "tasks").mockRejectedValueOnce(new Error("offline"));
    vi.spyOn(api, "repository").mockResolvedValue({ path: "/repo", branch: "main" });
    vi.spyOn(api, "agents").mockResolvedValue([]);
    vi.spyOn(api, "gitStatus").mockResolvedValue({ repository: { path: "/repo" }, files: [], branches: ["main"], current_branch: "main", clean: true, agent_running: false });

    render(<App />);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(screen.getByRole("alert").textContent).toContain("offline");

    fireEvent.click(screen.getByRole("button", { name: "Dismiss error" }));
    expect(screen.queryByRole("alert")).toBeNull();
  });
});
