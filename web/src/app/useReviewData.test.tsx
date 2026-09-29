import { act, cleanup, renderHook, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { api, type HandoffContext } from "./api";
import { useReviewData } from "./useReviewData";

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}

afterEach(() => { cleanup(); vi.restoreAllMocks(); });

describe("useReviewData task ownership", () => {
  it("surfaces a failed review snapshot instead of leaving it unhandled", async () => {
    vi.spyOn(api, "handoff").mockRejectedValue(new Error("daemon unavailable"));
    vi.spyOn(api, "graph").mockResolvedValue({ task_id: "task-1", nodes: [], edges: [] });
    vi.spyOn(api, "provenance").mockResolvedValue({ task_id: "task-1", records: [] });
    vi.spyOn(api, "verifications").mockResolvedValue([]);

    const { result } = renderHook(() => useReviewData("task-1"));

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

    const { result, rerender } = renderHook(({ taskId }) => useReviewData(taskId), { initialProps: { taskId: "task-old" } });
    rerender({ taskId: "task-new" });
    await waitFor(() => expect(result.current.handoff?.task_id).toBe("task-new"));

    await act(async () => { old.resolve(previous); await old.promise; });

    expect(result.current.handoff?.task_id).toBe("task-new");
  });
});
