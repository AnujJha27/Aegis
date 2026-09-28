import { act, renderHook, waitFor } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";
import { api } from "./api";
import type { AgentEvent } from "./api";
import * as eventTransport from "./events";
import { useAgentWorkspace } from "./useAgentWorkspace";

describe("useAgentWorkspace run lifecycle", () => {
  let emit: (event: AgentEvent) => void;

  beforeEach(() => {
    vi.restoreAllMocks();
    vi.spyOn(api, "events").mockResolvedValue([]);
    vi.spyOn(api, "runs").mockResolvedValue([{
      id: "run-1", taskId: "task-1", agent: "codex", status: "starting", startedAt: 10, finishedAt: 0,
    }]);
    vi.spyOn(eventTransport, "connectEvents").mockImplementation((onEvent) => {
      emit = onEvent;
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
});
