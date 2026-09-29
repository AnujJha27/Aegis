import { act, cleanup, renderHook, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { api, type HandoffContext, type VerificationRun } from "./api";
import { useReviewData } from "./useReviewData";

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}

afterEach(() => { cleanup(); vi.restoreAllMocks(); });

describe("useReviewData task ownership", () => {
  it("loads graph, activity, and handoff data only for the opened review view", async () => {
    const handoff = vi.spyOn(api, "handoff").mockResolvedValue({ task_id: "task-1", prompt: "task", recent_events: [], diff: "", changed_files: [], changed_files_truncated: false, verification: null, findings: [] });
    const graph = vi.spyOn(api, "graph").mockResolvedValue({ task_id: "task-1", nodes: [], edges: [] });
    const provenance = vi.spyOn(api, "provenance").mockResolvedValue({ task_id: "task-1", records: [] });
    const verifications = vi.spyOn(api, "verifications").mockResolvedValue([]);
    const { rerender } = renderHook(({ view }) => useReviewData("task-1", view), { initialProps: { view: undefined as "review" | "graphs" | "activity" | undefined } });
    await act(async () => {});
    expect(handoff).not.toHaveBeenCalled();
    expect(graph).not.toHaveBeenCalled();
    expect(provenance).not.toHaveBeenCalled();
    expect(verifications).not.toHaveBeenCalled();

    rerender({ view: "graphs" });
    await waitFor(() => expect(graph).toHaveBeenCalledOnce());
    expect(handoff).not.toHaveBeenCalled();
    expect(provenance).not.toHaveBeenCalled();
    expect(verifications).not.toHaveBeenCalled();
  });

  it("surfaces a failed review snapshot instead of leaving it unhandled", async () => {
    vi.spyOn(api, "handoff").mockRejectedValue(new Error("daemon unavailable"));
    vi.spyOn(api, "graph").mockResolvedValue({ task_id: "task-1", nodes: [], edges: [] });
    vi.spyOn(api, "provenance").mockResolvedValue({ task_id: "task-1", records: [] });
    vi.spyOn(api, "verifications").mockResolvedValue([]);

    const { result } = renderHook(() => useReviewData("task-1", "review"));

    await waitFor(() => expect(result.current.error).toBe("daemon unavailable"));
  });

  it("ignores an older task snapshot that resolves after the selected task", async () => {
    const old = deferred<HandoffContext>();
    const previous: HandoffContext = { task_id: "task-old", prompt: "old task", recent_events: [], diff: "", changed_files: [], changed_files_truncated: false, verification: null, findings: [] };
    const selected: HandoffContext = { ...previous, task_id: "task-new", prompt: "selected task" };
    vi.spyOn(api, "handoff").mockImplementation((taskId) => taskId === "task-old" ? old.promise : Promise.resolve(selected));
    vi.spyOn(api, "graph").mockResolvedValue({ task_id: "task-new", nodes: [], edges: [] });
    vi.spyOn(api, "provenance").mockResolvedValue({ task_id: "task-new", records: [] });
    vi.spyOn(api, "verifications").mockResolvedValue([]);

    const { result, rerender } = renderHook(({ taskId }) => useReviewData(taskId, "review"), { initialProps: { taskId: "task-old" } });
    rerender({ taskId: "task-new" });
    await waitFor(() => expect(result.current.handoff?.task_id).toBe("task-new"));

    await act(async () => { old.resolve(previous); await old.promise; });

    expect(result.current.handoff?.task_id).toBe("task-new");
  });

  it("does not let an older verification-history load erase a newly completed run", async () => {
    const oldHistory = deferred<VerificationRun[]>();
    vi.spyOn(api, "handoff").mockResolvedValue({ task_id: "task-1", prompt: "check", recent_events: [], diff: "", changed_files: [], changed_files_truncated: false, verification: null, findings: [] });
    vi.spyOn(api, "graph").mockResolvedValue({ task_id: "task-1", nodes: [], edges: [] });
    vi.spyOn(api, "provenance").mockResolvedValue({ task_id: "task-1", records: [] });
    vi.spyOn(api, "verifications").mockReturnValue(oldHistory.promise);
    const latest: VerificationRun = { id: "verify-new", taskId: "task-1", runId: null, command: ["ctest"], exitCode: 0, output: "passed", startedAt: 10, finishedAt: 12 };
    vi.spyOn(api, "verify").mockResolvedValue(latest);
    const { result } = renderHook(() => useReviewData("task-1", "review"));

    await act(async () => { await result.current.verify(); });
    expect(result.current.verification).toEqual(latest);

    await act(async () => { oldHistory.resolve([]); await oldHistory.promise; });
    expect(result.current.verification).toEqual(latest);
  });
});
