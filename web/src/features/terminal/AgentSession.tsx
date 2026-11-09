import { lazy, Suspense, useRef } from "react";
import type { AgentEvent, AgentRun, Task } from "../../app/api";
import { readable } from "../../app/events";
import type { PtyConnection } from "../../app/events";
import type { ReactNode } from "react";
import { AegisMark } from "../../components/AegisMark";
const AgentTerminal = lazy(() => import("./AgentTerminal"));

function importSummary(content: string): string | null {
  try {
    const parsed = JSON.parse(content) as { role?: unknown; text?: unknown; source_name?: unknown; source_line?: unknown; historical?: unknown };
    if (typeof parsed !== "object" || parsed === null || typeof parsed.text !== "string") return null;
    const role = typeof parsed.role === "string" ? parsed.role : "message";
    const source = typeof parsed.source_name === "string" ? (typeof parsed.source_line === "number" ? ` — source: ${parsed.source_name}:${parsed.source_line}` : ` — source: ${parsed.source_name}`) : "";
    const historical = parsed.historical === true ? " (historical)" : "";
    return `[imported ${role}${historical}] ${parsed.text}${source}`;
  } catch {
    return null;
  }
}

function cardText(event: AgentEvent): string {
  const raw = readable(event.content);
  return event.type === "import.message" ? importSummary(raw) ?? raw : raw;
}

export function AgentSession({ task, run, runFinished, turnBusy, turnCompleted, turnInterrupted, resumable, interactive, ptyConnection, onPtyInput, onPtyResize, events, prompt, onPrompt, onSend, busy, sending, children }: { task?: Task; run?: AgentRun; runFinished: boolean; turnBusy: boolean; turnCompleted: boolean; turnInterrupted: boolean; resumable: boolean; interactive: boolean; ptyConnection: PtyConnection; onPtyInput: (input: string) => void; onPtyResize: (cols: number, rows: number) => void; events: AgentEvent[]; prompt: string; onPrompt: (value: string) => void; onSend: () => void; busy: string; sending: boolean; children: ReactNode }) {
  const promptInput = useRef<HTMLTextAreaElement>(null);
  const cards = events.filter((event) => ["user.message", "agent.message.completed", "run.failed", "import.message"].includes(event.type) && readable(event.content).trim());
  const hasRunOutput = Boolean(run && events.some((event) => event.runId === run.id && readable(event.content).trim()));
  const showEmpty = run ? !interactive && !hasRunOutput : !cards.length;
  const stateLabel = !run ? "IDLE" : run.status === "failed" ? "FAILED" : run.status === "interrupted" ? "INTERRUPTED" : run.status === "terminated" ? "TERMINATED" : run.status === "completed" ? "COMPLETE" : run.status === "starting" ? "STARTING" : turnBusy ? "TURN BUSY · RUN ACTIVE" : turnInterrupted ? "TURN INTERRUPTED · RUN ACTIVE" : turnCompleted ? "TURN COMPLETE · RUN ACTIVE" : "RUN ACTIVE";
  const stateClass = !run ? "idle" : runFinished ? run.status : turnBusy ? "busy" : turnInterrupted ? "interrupted" : "running";
  const waitingForTurn = turnBusy || sending;
  const rawPrompt = task?.prompt ?? "";
  const firstLine = rawPrompt.split("\n")[0] ?? "";
  const headingText = rawPrompt ? (firstLine.length > 80 ? `${firstLine.slice(0, 80)}…` : rawPrompt.includes("\n") ? `${firstLine}…` : firstLine) : "Choose a task to begin";
  const nextAction = task?.context?.next_action;
  return <section className="session">
    <div className="session-heading">
      <div><span className="eyebrow">ACTIVE TASK</span><h1>{headingText}</h1>{nextAction && <p className="next-action">{nextAction}</p>}{rawPrompt && <details className="original-request"><summary>Original request</summary><pre>{rawPrompt}</pre></details>}</div>
      <span className={`session-id status-${stateClass}`} role="status" aria-live="polite">{run ? `${run.agent.toUpperCase()}${run.model ? ` · ${run.model}` : ""} · ${run.id.slice(0, 8)} · ${stateLabel}` : "IDLE"}</span>
    </div>
    <div className="output-surface">
      <div className={`terminal-wrap ${run && interactive ? "" : "terminal-hidden"}`}>
        <div className="terminal-label">LIVE PTY OUTPUT <span>{run?.agent ?? "terminal"}</span></div>
        {run && interactive && <Suspense fallback={<div className="code-state">Loading terminal…</div>}><AgentTerminal run={run} runFinished={runFinished} events={events} onPtyInput={onPtyInput} onPtyResize={onPtyResize} /></Suspense>}
      </div>
      {showEmpty && <div className="empty-output"><div className="empty-icon"><AegisMark /></div><h2>{run ? `${run.agent} is ready` : task ? "Ready for direction" : "Your workspace is ready"}</h2><p>{run ? interactive ? "The terminal is ready. Type directly into it to answer setup prompts or interact with the CLI." : "Send a prompt below to start the agent." : task ? "Launch an agent, then send a prompt from the dock below." : "Create a task on the left to start an agent run."}</p></div>}
      {cards.map((event) => <article className={`event-card ${event.type.includes("failed") ? "failed" : ""}`} key={event.id}><div className="event-meta"><span className={`event-dot ${event.type.includes("completed") || event.type.includes("finished") ? "done" : ""}`} /><span>{event.agent || "system"}</span><span>{event.type.replaceAll(".", " / ")}</span><time>{new Date(event.timestamp).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}</time></div><pre>{cardText(event)}</pre></article>)}
    </div>
    <div className="composer">
      <div className="composer-tools">{children}</div>
      {interactive ? <div className="composer-hint pty-hint">{runFinished ? "Run ended · terminal is read-only" : ptyConnection === "connected" ? "Interactive CLI · click the terminal or start typing to answer prompts (e.g. project trust)" : ptyConnection === "connecting" ? "Connecting interactive terminal…" : `Terminal ${ptyConnection === "unavailable" ? "offline · retrying" : "reconnecting…"} · input paused`}<span className="composer-status"><i />{busy}</span></div> : <>
        <div className="prompt-row"><textarea ref={promptInput} aria-label="Message the active agent" value={prompt} onChange={(event) => onPrompt(event.target.value)} onKeyDown={(event) => { if (event.key === "Enter" && !event.shiftKey) { event.preventDefault(); onSend(); } }} placeholder={turnCompleted && resumable ? "Continue this run with a new prompt…" : run && !runFinished ? "Message the active agent…" : "Launch an agent to start prompting…"} disabled={!run || runFinished || waitingForTurn} />{run && !runFinished && turnCompleted && resumable && <button className="continue-button" onClick={() => promptInput.current?.focus()}>Continue run</button>}<button className="send-button" onClick={onSend} disabled={!run || runFinished || waitingForTurn || !prompt.trim()}>{waitingForTurn ? "…" : "Send"}<span>↗</span></button></div>
        <div className="composer-hint"><span>Enter to send</span><span>Shift + Enter for newline</span><span className="composer-status"><i />{busy}</span></div>
      </>}
    </div>
  </section>;
}
