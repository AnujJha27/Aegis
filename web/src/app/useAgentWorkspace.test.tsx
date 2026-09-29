import { act, renderHook, waitFor } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";
import { api } from "./api";
import type { AgentEvent } from "./api";
import * as eventTransport from "./events";
import type { EventConnection } from "./events";
import { useAgentWorkspace } from "./useAgentWorkspace";

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}

describe("useAgentWorkspace run lifecycle", () => {
  let emit: (event: AgentEvent) => void;
  let updateConnection: (state: EventConnection, reconnected: boolean) => void;

  beforeEach(() => {
    vi.restoreAllMocks();
    vi.spyOn(api, "events").mockResolvedValue([]);
    vi.spyOn(api, "runs").mockResolvedValue([{
      id: "run-1", taskId: "task-1", agent: "codex", status: "starting", startedAt: 10, finishedAt: 0,
    }]);
    vi.spyOn(eventTransport, "connectEvents").mockImplementation((onEvent, onState) => {
      emit = onEvent;
      updateConnection = onState;
      return () => {};
    });
  });

  it("maps run.started to the persisted running status", async () => {
    const { result } = renderHook(() => useAgentWorkspace("task-1"));
    await waitFor(() => expect(result.current.runs[0]?.status).toBe("starting"));

    act(() => emit({
      id: "event-1", taskId: "task-1", runId: "run-1", type: "run.started",
      agent: "codex", content: "", timestamp: 20,
    }));

    await waitFor(() => expect(result.current.runs[0]?.status).toBe("running"));
    expect(result.current.runs[0]?.finishedAt).toBe(0);
  });

  it("keeps live events that arrive while reconnect history is loading", async () => {
    let finishHistory!: (events: AgentEvent[]) => void;
    vi.spyOn(api, "events")
      .mockResolvedValueOnce([])
      .mockImplementationOnce(() => new Promise((resolve) => { finishHistory = resolve; }));
    const { result } = renderHook(() => useAgentWorkspace("task-1"));
    await waitFor(() => expect(api.events).toHaveBeenCalledTimes(1));

    act(() => updateConnection("connected", true));
    const liveEvent: AgentEvent = {
      id: "event-live", taskId: "task-1", runId: "run-1", type: "agent.message.completed",
      agent: "codex", content: "Recovered output", timestamp: 30,
    };
    act(() => emit(liveEvent));
    await waitFor(() => expect(result.current.currentEvents).toContainEqual(liveEvent));

    await act(async () => { finishHistory([]); await Promise.resolve(); });

    expect(result.current.currentEvents).toContainEqual(liveEvent);
  });

  it("ignores an older reconnect snapshot that completes after a newer one", async () => {
    const older = deferred<AgentEvent[]>();
    const newer = deferred<AgentEvent[]>();
    vi.spyOn(api, "events").mockResolvedValueOnce([])
      .mockImplementationOnce(() => older.promise)
      .mockImplementationOnce(() => newer.promise);
    const { result } = renderHook(() => useAgentWorkspace("task-1"));
    await waitFor(() => expect(api.events).toHaveBeenCalledTimes(1));

    act(() => { updateConnection("connected", true); updateConnection("connected", true); });
    const liveEvent: AgentEvent = {
      id: "event-live", taskId: "task-1", runId: "run-1", type: "agent.message.completed",
      agent: "codex", content: "Latest output", timestamp: 30,
    };
    act(() => emit(liveEvent));
    await waitFor(() => expect(api.events).toHaveBeenCalledTimes(3));
    await act(async () => { newer.resolve([]); await Promise.resolve(); });
    await act(async () => { older.resolve([]); await Promise.resolve(); });

    expect(result.current.currentEvents).toContainEqual(liveEvent);
  });
});
