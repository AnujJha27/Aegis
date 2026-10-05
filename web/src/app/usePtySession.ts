import { useEffect, useRef, useState } from "react";
import { agentEvent } from "./api";
import { connectPty, connectTerminal } from "./events";
import type { PtyConnection } from "./events";
import type { AgentRun } from "./api";

export function usePtySession(run?: AgentRun, interactive = false, finished = false) {
  const socket = useRef<WebSocket | null>(null);
  const pending = useRef<string[]>([]);
  const firstConnection = useRef(true);
  const size = useRef({ cols: 100, rows: 30 });
  const [error, setError] = useState("");
  const [connection, setConnection] = useState<PtyConnection>("idle");

  useEffect(() => {
    if (!run || !interactive) return;
    let active = true;
    let retries = 0;
    let retryTimer = 0;
    let terminal: WebSocket | undefined;
    const connect = () => {
      if (!active) return;
      const current = connectTerminal(run.id);
      terminal = current;
      current.onmessage = (message) => {
        try {
          const event = agentEvent(JSON.parse(message.data) as Record<string, unknown>);
          if (event.runId === run.id && ["terminal.output", "terminal.replay_truncated", "stream.resync_required"].includes(event.type))
            window.dispatchEvent(new CustomEvent("aegis:terminal-output", { detail: event }));
        } catch { /* The next reconnect replays persisted terminal output. */ }
      };
      current.onerror = () => current.close();
      current.onopen = () => { retries = 0; };
      current.onclose = () => {
        if (!active || terminal !== current) return;
        retryTimer = window.setTimeout(connect, Math.min(500 * 2 ** retries++, 8000));
      };
    };
    const replay = () => { if (!active) return; window.clearTimeout(retryTimer); terminal?.close(); connect(); };
    window.addEventListener("aegis:terminal-ready", replay);
    connect();
    return () => { active = false; window.removeEventListener("aegis:terminal-ready", replay); window.clearTimeout(retryTimer); terminal?.close(); };
  }, [run?.id, interactive]);

  useEffect(() => {
    if (!run || !interactive || finished) {
      socket.current?.close();
      socket.current = null;
      pending.current = [];
      setConnection("idle");
      return;
    }
    pending.current = [];
    firstConnection.current = true;
    let active = true;
    let retries = 0;
    let retryTimer = 0;
    const connect = () => {
      if (!active) return;
      setConnection(retries > 4 ? "unavailable" : retries ? "reconnecting" : "connecting");
      const current = connectPty(run.id);
      socket.current = current;
      current.onopen = () => {
        if (!active || socket.current !== current) return;
        retries = 0;
        setConnection("connected");
        current.send(JSON.stringify({ type: "resize", ...size.current }));
        if (firstConnection.current) {
          firstConnection.current = false;
          for (const input of pending.current) current.send(input);
          pending.current = [];
        }
      };
      current.onerror = () => current.close();
      current.onclose = () => {
        if (!active || socket.current !== current) return;
        socket.current = null;
        pending.current = [];
        firstConnection.current = false;
        const delay = Math.min(500 * 2 ** retries++, 8000);
        setConnection(retries > 4 ? "unavailable" : "reconnecting");
        retryTimer = window.setTimeout(connect, delay);
      };
    };
    connect();
    return () => {
      active = false;
      window.clearTimeout(retryTimer);
      socket.current?.close();
      socket.current = null;
    };
  }, [run?.id, interactive, finished]);

  function send(input: string) {
    if (socket.current?.readyState === WebSocket.OPEN) socket.current.send(input);
    else if (firstConnection.current && socket.current?.readyState === WebSocket.CONNECTING) {
      if (pending.current.length >= 128 || pending.current.reduce((sum, chunk) => sum + chunk.length, 0) + input.length > 16384) {
        setError("Terminal input queue is full while connecting. Wait for the terminal and retry the last input."); return;
      }
      pending.current.push(input);
    }
  }

  function resize(cols: number, rows: number) {
    size.current = { cols, rows };
    if (socket.current?.readyState === WebSocket.OPEN) socket.current.send(JSON.stringify({ type: "resize", cols, rows }));
  }

  return { send, resize, connection, error };
}
