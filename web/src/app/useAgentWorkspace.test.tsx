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

  it("keeps the managed-browser presence socket connected with no selected task", async () => {
    const { result } = renderHook(() => useAgentWorkspace());
    expect(eventTransport.connectEvents).toHaveBeenCalledTimes(1);
    expect(api.events).not.toHaveBeenCalled();
    expect(api.runs).not.toHaveBeenCalled();
    act(() => updateConnection("connected", false));
    expect(result.current.connection).toBe("connected");
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

  it("restores the active run and busy turn after a browser refresh", async () => {
    vi.spyOn(api, "runs").mockResolvedValue([{ id: "run-1", taskId: "task-1", agent: "codex", status: "running", startedAt: 10, finishedAt: 0 }]);
    vi.spyOn(api, "events").mockResolvedValue([{ id: "turn-live", taskId: "task-1", runId: "run-1", type: "turn.started", agent: "codex", content: "", timestamp: 22 }]);
    const { result } = renderHook(() => useAgentWorkspace("task-1"));
    await waitFor(() => expect(result.current.currentRun?.status).toBe("running"));
    expect(result.current.turnBusy).toBe(true);
    expect(result.current.currentRunEvents.map((event) => event.id)).toContain("turn-live");
  });

  it("keeps a completed turn resumable inside its still-running run", async () => {
    const { result } = renderHook(() => useAgentWorkspace("task-1"));
    await waitFor(() => expect(result.current.runs[0]?.status).toBe("starting"));
    act(() => emit({ id: "start", taskId: "task-1", runId: "run-1", type: "run.started", agent: "codex", content: "", timestamp: 20 }));
    act(() => emit({ id: "turn-start", taskId: "task-1", runId: "run-1", type: "turn.started", agent: "codex", content: "", timestamp: 21 }));
    expect(result.current.turnBusy).toBe(true);

    act(() => emit({ id: "turn-done", taskId: "task-1", runId: "run-1", type: "turn.completed", agent: "codex", content: "", timestamp: 22 }));

    expect(result.current.runs[0]?.status).toBe("running");
    expect(result.current.runFinished).toBe(false);
    expect(result.current.turnBusy).toBe(false);
    expect(result.current.turnCompleted).toBe(true);

    act(() => emit({ id: "turn-stop", taskId: "task-1", runId: "run-1", type: "turn.interrupted", agent: "codex", content: "", timestamp: 23 }));
    expect(result.current.runFinished).toBe(false);
    expect(result.current.turnCompleted).toBe(false);
    expect(result.current.turnInterrupted).toBe(true);
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

  it("keeps terminal history for initial replay but excludes live PTY chunks from React state", async () => {
    const historical: AgentEvent = {
      id: "pty-history", taskId: "task-1", runId: "run-1", type: "terminal.output",
      agent: "claude", content: "history", timestamp: 15,
    };
    vi.spyOn(api, "events").mockResolvedValue([historical]);
    const onEvent = vi.fn();
    const { result } = renderHook(() => useAgentWorkspace("task-1", onEvent));
    await waitFor(() => expect(result.current.currentEvents).toContainEqual(historical));

    const live: AgentEvent = { ...historical, id: "pty-live", content: "live", timestamp: 20 };
    act(() => emit(live));
    expect(onEvent).toHaveBeenCalledWith(live);
    expect(result.current.currentEvents).not.toContainEqual(live);
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
