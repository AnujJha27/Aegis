import { useEffect, useRef } from "react";
import { connectPty } from "./events";
import type { AgentRun } from "./api";

export function usePtySession(run?: AgentRun, interactive = false, finished = false) {
  const socket = useRef<WebSocket | null>(null);
  const pending = useRef<string[]>([]);
  const size = useRef({ cols: 100, rows: 30 });

  useEffect(() => {
    if (!run || !interactive || finished) {
      socket.current?.close();
      socket.current = null;
      return;
    }
    pending.current = [];
    const current = connectPty(run.id);
    socket.current = current;
    current.onopen = () => {
      current.send(JSON.stringify({ type: "resize", ...size.current }));
      for (const input of pending.current) current.send(input);
      pending.current = [];
    };
    return () => {
      current.close();
      if (socket.current === current) socket.current = null;
    };
  }, [run?.id, interactive, finished]);

  function send(input: string) {
    if (socket.current?.readyState === WebSocket.OPEN) socket.current.send(input);
    else if (socket.current?.readyState === WebSocket.CONNECTING) pending.current.push(input);
  }

  function resize(cols: number, rows: number) {
    size.current = { cols, rows };
    if (socket.current?.readyState === WebSocket.OPEN) socket.current.send(JSON.stringify({ type: "resize", cols, rows }));
  }

  return { send, resize };
}
