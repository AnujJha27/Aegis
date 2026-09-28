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
  send = vi.fn();
  close = vi.fn();
  open() { this.readyState = 1; this.onopen?.(); }
  drop() { this.readyState = 3; this.onclose?.(); }
}

describe("usePtySession reconnect", () => {
  const sockets: FakeSocket[] = [];
  const run: AgentRun = { id: "run-1", taskId: "task-1", agent: "claude", status: "running", startedAt: 1, finishedAt: 0 };

  beforeEach(() => {
    sockets.length = 0;
    vi.useFakeTimers();
    vi.stubGlobal("WebSocket", { OPEN: 1, CONNECTING: 0 });
    vi.spyOn(eventTransport, "connectPty").mockImplementation(() => {
      const socket = new FakeSocket();
      sockets.push(socket);
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
    act(() => sockets[0].drop());
    expect(result.current.connection).toBe("reconnecting");

    await act(async () => { await vi.advanceTimersByTimeAsync(500); });
    expect(sockets).toHaveLength(2);
    act(() => sockets[1].open());
    expect(result.current.connection).toBe("connected");
    expect(sockets[1].send).toHaveBeenCalledWith(JSON.stringify({ type: "resize", cols: 100, rows: 30 }));
    unmount();
  });
});
