import { act, renderHook } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { AgentRun } from "./api";
import * as eventTransport from "./events";
import { usePtySession } from "./usePtySession";

class FakeSocket {
  readyState = 0;
  onopen: (() => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  onmessage: ((message: { data: string }) => void) | null = null;
  send = vi.fn();
  close = vi.fn();
  open() { this.readyState = 1; this.onopen?.(); }
  drop() { this.readyState = 3; this.onclose?.(); }
}

describe("usePtySession reconnect", () => {
  const sockets: FakeSocket[] = [];
  const terminalSockets: FakeSocket[] = [];
  const run: AgentRun = { id: "run-1", taskId: "task-1", agent: "claude", status: "running", startedAt: 1, finishedAt: 0 };

  beforeEach(() => {
    sockets.length = 0;
    terminalSockets.length = 0;
    vi.useFakeTimers();
    vi.stubGlobal("WebSocket", { OPEN: 1, CONNECTING: 0 });
    vi.spyOn(eventTransport, "connectPty").mockImplementation(() => {
      const socket = new FakeSocket();
      sockets.push(socket);
      return socket as unknown as WebSocket;
    });
    vi.spyOn(eventTransport, "connectTerminal").mockImplementation(() => {
      const socket = new FakeSocket();
      terminalSockets.push(socket);
      return socket as unknown as WebSocket;
    });
  });
  afterEach(() => { vi.useRealTimers(); vi.restoreAllMocks(); vi.unstubAllGlobals(); });

  it("reconnects a live run after the PTY socket drops", async () => {
    const { result, unmount } = renderHook(() => usePtySession(run, true, false));
    expect(result.current.connection).toBe("connecting");
    expect(sockets).toHaveLength(1);

    act(() => sockets[0].open());
    expect(result.current.connection).toBe("connected");
    act(() => result.current.send("typed once"));
    expect(sockets[0].send).toHaveBeenCalledWith("typed once");
    act(() => sockets[0].drop());
    expect(result.current.connection).toBe("reconnecting");
    act(() => result.current.send("do not replay after disconnect"));

    await act(async () => { await vi.advanceTimersByTimeAsync(500); });
    expect(sockets).toHaveLength(2);
    act(() => sockets[1].open());
    expect(result.current.connection).toBe("connected");
    expect(sockets[1].send).toHaveBeenCalledWith(JSON.stringify({ type: "resize", cols: 100, rows: 30 }));
    expect(sockets[1].send).not.toHaveBeenCalledWith("typed once");
    expect(sockets[1].send).not.toHaveBeenCalledWith("do not replay after disconnect");
    unmount();
  });

  it("reconnects terminal output separately and reports replay gaps", async () => {
    const received = vi.fn();
    window.addEventListener("aegis:terminal-output", received);
    const { unmount } = renderHook(() => usePtySession(run, true, false));
    expect(terminalSockets).toHaveLength(1);
    act(() => terminalSockets[0].open());
    act(() => terminalSockets[0].onmessage?.({ data: JSON.stringify({ id: "gap", task_id: run.taskId, run_id: run.id, type: "terminal.replay_truncated", agent: "claude", content: "earlier output was discarded", timestamp: 2 }) }));
    expect(received).toHaveBeenCalledWith(expect.objectContaining({ detail: expect.objectContaining({ type: "terminal.replay_truncated", runId: run.id }) }));

    act(() => terminalSockets[0].drop());
    await act(async () => { await vi.advanceTimersByTimeAsync(500); });
    expect(terminalSockets).toHaveLength(2);
    unmount();
    window.removeEventListener("aegis:terminal-output", received);
  });
});
