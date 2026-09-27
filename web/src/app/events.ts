import { agentEvent, type AgentEvent } from "./api";

export type EventConnection = "connecting" | "connected" | "reconnecting" | "unavailable";

export function connectEvents(onEvent: (event: AgentEvent) => void, onState: (state: EventConnection, reconnected: boolean) => void): () => void {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  let socket: WebSocket | undefined;
  let retryTimer = 0;
  let closed = false;
  let retries = 0;
  let connectedOnce = false;
  const connect = () => {
    if (closed) return;
    onState(retries > 4 ? "unavailable" : connectedOnce ? "reconnecting" : "connecting", false);
    socket = new WebSocket(`${protocol}//${window.location.host}/ws/events`);
    socket.onopen = () => {
      const reconnected = connectedOnce;
      connectedOnce = true;
      retries = 0;
      onState("connected", reconnected);
    };
    socket.onmessage = (message) => {
      try { onEvent(agentEvent(JSON.parse(message.data) as Record<string, unknown>)); }
      catch { /* Persisted event history remains authoritative. */ }
    };
    socket.onerror = () => socket?.close();
    socket.onclose = () => {
      if (closed) return;
      const delay = Math.min(500 * 2 ** retries++, 8000);
      onState(retries > 4 ? "unavailable" : "reconnecting", false);
      retryTimer = window.setTimeout(connect, delay);
    };
  };
  connect();
  return () => { closed = true; window.clearTimeout(retryTimer); socket?.close(); };
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
