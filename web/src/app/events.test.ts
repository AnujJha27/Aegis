import { afterEach, describe, expect, it, vi } from "vitest";
import { connectEvents } from "./events";

class FakeSocket {
  static instances: FakeSocket[] = [];
  onopen: (() => void) | null = null;
  onmessage: ((message: { data: string }) => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  close = vi.fn(() => this.onclose?.());
  constructor(readonly url: string) { FakeSocket.instances.push(this); }
  open() { this.onopen?.(); }
  drop() { this.onclose?.(); }
  message(data: string) { this.onmessage?.({ data }); }
}

afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); FakeSocket.instances = []; });

describe("event WebSocket reconnect", () => {
  it("reconnects with persisted-history recovery and resumes live delivery", async () => {
    vi.useFakeTimers();
    vi.stubGlobal("WebSocket", Object.assign(FakeSocket, { OPEN: 1, CONNECTING: 0 }));
    const onEvent = vi.fn();
    const onState = vi.fn();
    const disconnect = connectEvents(onEvent, onState);
    expect(FakeSocket.instances).toHaveLength(1);
    FakeSocket.instances[0].open();
    FakeSocket.instances[0].drop();
    await vi.advanceTimersByTimeAsync(500);
    expect(FakeSocket.instances).toHaveLength(2);
    FakeSocket.instances[1].open();
    expect(onState).toHaveBeenLastCalledWith("connected", true);

    FakeSocket.instances[1].message(JSON.stringify({ id: "event-1", task_id: "task-1", run_id: "run-1", type: "turn.completed", agent: "codex", content: "", timestamp: 1 }));
    expect(onEvent).toHaveBeenCalledWith(expect.objectContaining({ id: "event-1", taskId: "task-1", runId: "run-1" }));
    disconnect();
  });
});
