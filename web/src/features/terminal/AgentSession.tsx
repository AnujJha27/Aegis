import { useEffect, useRef } from "react";
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import "@xterm/xterm/css/xterm.css";
import type { AgentEvent, AgentRun, Task } from "../../app/api";
import { readable } from "../../app/events";
import type { ReactNode } from "react";

export function AgentSession({ task, run, events, prompt, onPrompt, onSend, busy, children }: { task?: Task; run?: AgentRun; events: AgentEvent[]; prompt: string; onPrompt: (value: string) => void; onSend: () => void; busy: string; children: ReactNode }) {
  const terminalHost = useRef<HTMLDivElement>(null);
  const terminal = useRef<Terminal | undefined>(undefined);
  const fit = useRef<FitAddon | undefined>(undefined);
  const rendered = useRef(new Set<string>());
  const activeRun = useRef("");

  useEffect(() => {
    if (!terminalHost.current) return;
    const next = new Terminal({ convertEol: true, cursorBlink: false, disableStdin: true, fontFamily: "SFMono-Regular, Consolas, monospace", fontSize: 12, theme: { background: "#0b0f15", foreground: "#d5deea", cursor: "#9fe870" } });
    const addon = new FitAddon();
    next.loadAddon(addon);
    next.open(terminalHost.current);
    addon.fit();
    terminal.current = next;
    fit.current = addon;
    const observer = new ResizeObserver(() => addon.fit());
    observer.observe(terminalHost.current);
    return () => { observer.disconnect(); next.dispose(); terminal.current = undefined; fit.current = undefined; };
  }, []);

  useEffect(() => {
    if (!terminal.current) return;
    if (activeRun.current !== (run?.id ?? "")) {
      activeRun.current = run?.id ?? "";
      rendered.current.clear();
      terminal.current.clear();
    }
    for (const event of events) {
      if (event.runId !== run?.id || event.type !== "agent.message.delta" || rendered.current.has(event.id)) continue;
      terminal.current.write(event.content);
      rendered.current.add(event.id);
    }
  }, [events, run?.id]);

  const cards = events.filter((event) => event.type !== "agent.message.delta");
  return <section className="session"><div className="session-heading"><div><span className="eyebrow">ACTIVE TASK</span><h1>{task?.prompt ?? "Choose a task to begin"}</h1></div><span className="session-id">{run ? `RUN ${run.id.slice(0, 8)}` : "IDLE"}</span></div><div className="output-surface">{run && <div className="terminal-wrap"><div className="terminal-label">LIVE PTY OUTPUT <span>{run.agent}</span></div><div className="xterm-host" ref={terminalHost} /></div>}{!cards.length && !run && <div className="empty-output"><div className="empty-icon">✦</div><h2>{task ? "Ready for direction" : "Your workspace is ready"}</h2><p>{task ? "Launch an agent, then send a prompt from the dock below." : "Create a task on the left to start an agent run."}</p></div>}{cards.map((event) => <article className={`event-card ${event.type.includes("failed") ? "failed" : ""}`} key={event.id}><div className="event-meta"><span className={`event-dot ${event.type.includes("completed") || event.type.includes("finished") ? "done" : ""}`} /><span>{event.agent || "system"}</span><span>{event.type.replaceAll(".", " / ")}</span><time>{new Date(event.timestamp * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}</time></div><pre>{readable(event.content)}</pre></article>)}</div><div className="composer"><div className="composer-tools">{children}</div><div className="prompt-row"><textarea value={prompt} onChange={(event) => onPrompt(event.target.value)} onKeyDown={(event) => { if (event.key === "Enter" && !event.shiftKey) { event.preventDefault(); onSend(); } }} placeholder={run ? "Message the active agent…" : "Launch an agent to start prompting…"} disabled={!run} /><button className="send-button" onClick={onSend} disabled={!run || !prompt.trim()}>{busy.includes("working") ? "…" : "Send"}<span>↗</span></button></div><div className="composer-hint"><span>Enter to send</span><span>Shift + Enter for newline</span><span className="composer-status"><i />{busy}</span></div></div></section>;
}
