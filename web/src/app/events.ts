import { agentEvent, type AgentEvent } from "./api";

export function connectEvents(onEvent: (event: AgentEvent) => void): () => void {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const socket = new WebSocket(`${protocol}//${window.location.host}/ws/events`);
  socket.onmessage = (message) => {
    try {
      onEvent(agentEvent(JSON.parse(message.data) as Record<string, unknown>));
    } catch {
      // Ignore malformed event frames; the persisted event list remains authoritative.
    }
  };
  return () => socket.close();
}

export function connectPty(runId: string): WebSocket {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  return new WebSocket(`${protocol}//${window.location.host}/ws/pty/${encodeURIComponent(runId)}`);
}

export function readable(content: string): string {
  return content
    .replace(/\x1b\][^\x07]*(?:\x07|\x1b\\)/g, "")
    .replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, "")
    .replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g, "");
}
