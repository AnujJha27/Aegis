import { useEffect, useRef } from "react";
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import "@xterm/xterm/css/xterm.css";
import type { AgentEvent, AgentRun } from "../../app/api";
export default function AgentTerminal({ run, runFinished, events, onPtyInput, onPtyResize }: { run: AgentRun; runFinished: boolean; events: AgentEvent[]; onPtyInput: (input: string) => void; onPtyResize: (cols: number, rows: number) => void }) {
  const host = useRef<HTMLDivElement>(null), terminal = useRef<Terminal | undefined>(undefined);
  const handlers = useRef({ onPtyInput, onPtyResize, runFinished }); handlers.current = { onPtyInput, onPtyResize, runFinished };
  const rendered = useRef(new Set<string>()), activeRun = useRef(run.id);
  const write = (event: AgentEvent) => {
    if (event.runId !== activeRun.current || !["terminal.output", "terminal.replay_truncated", "stream.resync_required"].includes(event.type) || rendered.current.has(event.id)) return;
    terminal.current?.write(event.type === "terminal.output" ? event.content : `\r\n[aegis: ${event.content}]\r\n`);
    rendered.current.add(event.id); if (rendered.current.size > 4096) rendered.current.delete(rendered.current.values().next().value!);
  };
  useEffect(() => {
    const next = new Terminal({ convertEol: true, cursorBlink: false, scrollback: 3000, fontFamily: "SFMono-Regular, Consolas, monospace", fontSize: 12, theme: { background: "#151a21", foreground: "#d5deea", cursor: "#91a9c4" } });
    const fit = new FitAddon(); next.loadAddon(fit); next.open(host.current!); terminal.current = next;
    const resize = () => { fit.fit(); const size = fit.proposeDimensions(); if (size) handlers.current.onPtyResize(size.cols, size.rows); };
    resize(); next.onData((data) => { if (!handlers.current.runFinished) handlers.current.onPtyInput(data); });
    const observer = new ResizeObserver(resize); observer.observe(host.current!);
    const receive = (event: Event) => write((event as CustomEvent<AgentEvent>).detail);
    window.addEventListener("aegis:terminal-output", receive);
    window.dispatchEvent(new Event("aegis:terminal-ready"));
    return () => { window.removeEventListener("aegis:terminal-output", receive); observer.disconnect(); next.dispose(); terminal.current = undefined; };
  }, []);
  useEffect(() => {
    if (activeRun.current !== run.id) { activeRun.current = run.id; rendered.current.clear(); terminal.current?.clear(); }
    if (!runFinished) terminal.current?.focus();
    for (const event of events) write(event);
  }, [run.id, events, runFinished]);
  return <div className="xterm-host" ref={host} onClick={() => terminal.current?.focus()} />;
}
