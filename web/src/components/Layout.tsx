import type { Agent, AgentEvent, AgentRun, GitChange, GitStatus, HandoffContext, ProvenanceRecord, Repository, Task, TaskGraph, VerificationRun } from "../app/api";
import type { PtyConnection } from "../app/events";
import { AgentPicker } from "../features/agents/AgentPicker";
import { lazy, Suspense } from "react";
const ReviewPanel = lazy(() => import("../features/review/ReviewPanel").then((module) => ({ default: module.ReviewPanel })));
import { TaskList } from "../features/tasks/TaskList";
import { AgentSession } from "../features/terminal/AgentSession";
import { VerificationPanel } from "../features/verification/VerificationPanel";

type Props = {
  repository?: Repository; tasks: Task[]; selectedTask?: Task; onSelectTask: (task: Task) => void;
  taskPrompt: string; onTaskPrompt: (value: string) => void; onCreateTask: () => void;
  agents: Agent[]; selectedAgent: string; onAgentChange: (agent: string) => void; onLaunch: () => void;
  run?: AgentRun; runs: AgentRun[]; onSelectRun: (id: string) => void; onDeleteRun: (id: string) => void; runFinished: boolean; onPtyInput: (input: string) => void; onPtyResize: (cols: number, rows: number) => void; ptyConnection: PtyConnection; events: AgentEvent[]; activityEvents: AgentEvent[]; prompt: string; onPrompt: (value: string) => void; onSend: () => void; busy: string;
  screen: "session" | "review"; onScreen: (screen: "session" | "review") => void; connection: string;
  drawer: "review" | "graphs" | "activity"; onDrawer: (drawer: "review" | "graphs" | "activity") => void;
  gitChanges: GitChange[]; gitStatus?: GitStatus; onGitChanged: () => Promise<void>; verification?: VerificationRun; handoff?: HandoffContext; graph?: TaskGraph; provenance: ProvenanceRecord[]; onVerify: () => void; error: string; onDismissError: () => void; openFilePath?: string; onOpenFile: (path: string) => void;
};

export function Layout(props: Props) {
  return <main className="app-shell">
    <header className="topbar"><div className="brand"><span className="brand-mark">✦</span><span>Aegis</span><small>LOCAL CONTROL PLANE</small></div><div className="repo"><span className="pulse" />{props.repository?.branch ?? "opening repository"}<span className="repo-path">{props.repository?.path ?? ""}</span></div><div className="screen-toggle" role="group" aria-label="Workspace screen"><button aria-pressed={props.screen === "session"} className={props.screen === "session" ? "active" : ""} onClick={() => props.onScreen("session")}>Session</button><button aria-pressed={props.screen === "review"} className={props.screen === "review" ? "active" : ""} onClick={() => props.onScreen("review")}>Review</button></div><div className={`connection-status ${props.connection}`}><i />{props.connection === "connected" ? "Connected" : props.connection === "reconnecting" ? "Reconnecting…" : props.connection === "unavailable" ? "Daemon unavailable" : "Connecting…"}</div><div className="top-status">{props.busy}</div></header>
    {props.screen === "session" ? <section className="workspace">
      <TaskList tasks={props.tasks} selectedTask={props.selectedTask} onSelect={props.onSelectTask} prompt={props.taskPrompt} onPrompt={props.onTaskPrompt} onCreate={props.onCreateTask} />
      <AgentSession task={props.selectedTask} run={props.run} runs={props.runs} runFinished={props.runFinished} interactive={Boolean(props.agents.find((agent) => agent.name === props.run?.agent)?.interactive)} ptyConnection={props.ptyConnection} onPtyInput={props.onPtyInput} onPtyResize={props.onPtyResize} events={props.events} prompt={props.prompt} onPrompt={props.onPrompt} onSend={props.onSend} busy={props.busy}>
        <AgentPicker agents={props.agents} selected={props.selectedAgent} onChange={props.onAgentChange} onLaunch={props.onLaunch} runs={props.runs} run={props.run} runFinished={props.runFinished} onSelectRun={props.onSelectRun} onDeleteRun={props.onDeleteRun} />
      </AgentSession>
    </section> : <><nav className="review-tabs" aria-label="Review views"><button aria-pressed={props.drawer === "review"} className={props.drawer === "review" ? "active" : ""} onClick={() => props.onDrawer("review")}>Review</button><button aria-pressed={props.drawer === "graphs"} className={props.drawer === "graphs" ? "active" : ""} onClick={() => props.onDrawer("graphs")}>Graphs</button><button aria-pressed={props.drawer === "activity"} className={props.drawer === "activity" ? "active" : ""} onClick={() => props.onDrawer("activity")}>Activity</button></nav><Suspense fallback={<div className="code-state">Loading review workspace…</div>}><ReviewPanel view={props.drawer} taskId={props.selectedTask?.id} activeRunId={props.run?.id} taskPrompt={props.selectedTask?.prompt} events={props.activityEvents} graph={props.graph} provenance={props.provenance} handoff={props.handoff} gitChanges={props.gitChanges} gitStatus={props.gitStatus} onGitChanged={props.onGitChanged} verification={props.verification} verificationRunning={props.busy === "Verifying…"} onVerify={props.onVerify} openFilePath={props.openFilePath} onOpenFile={props.onOpenFile} /></Suspense></>}
    {props.error && <div className="error-toast" role="alert"><span>{props.error}</span><button aria-label="Dismiss error" onClick={props.onDismissError}>×</button></div>}
  </main>;
}
