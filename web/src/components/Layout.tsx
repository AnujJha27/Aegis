import type { Agent, AgentEvent, AgentRun, GitChange, GitStatus, HandoffContext, ProvenanceRecord, Repository, Task, TaskGraph, VerificationRun } from "../app/api";
import { AgentPicker } from "../features/agents/AgentPicker";
import { ReviewPanel } from "../features/review/ReviewPanel";
import { TaskList } from "../features/tasks/TaskList";
import { AgentSession } from "../features/terminal/AgentSession";
import { VerificationPanel } from "../features/verification/VerificationPanel";

type Props = {
  repository?: Repository; tasks: Task[]; selectedTask?: Task; onSelectTask: (task: Task) => void;
  taskPrompt: string; onTaskPrompt: (value: string) => void; onCreateTask: () => void;
  agents: Agent[]; selectedAgent: string; onAgentChange: (agent: string) => void; onLaunch: () => void;
  run?: AgentRun; runs: AgentRun[]; onSelectRun: (id: string) => void; runFinished: boolean; onPtyInput: (input: string) => void; events: AgentEvent[]; prompt: string; onPrompt: (value: string) => void; onSend: () => void; busy: string;
  screen: "session" | "review"; onScreen: (screen: "session" | "review") => void;
  drawer: "review" | "graphs" | "activity" | "handoff"; onDrawer: (drawer: "review" | "graphs" | "activity" | "handoff") => void;
  diff: string; gitChanges: GitChange[]; gitStatus?: GitStatus; onGitChanged: () => Promise<void>; verification?: VerificationRun; handoff?: HandoffContext; graph?: TaskGraph; provenance: ProvenanceRecord[]; onVerify: () => void; error: string;
};

export function Layout(props: Props) {
  return <main className="app-shell">
    <header className="topbar"><div className="brand"><span className="brand-mark">✦</span><span>Aegis</span><small>LOCAL CONTROL PLANE</small></div><div className="repo"><span className="pulse" />{props.repository?.branch ?? "opening repository"}<span className="repo-path">{props.repository?.path ?? ""}</span></div><div className="screen-toggle"><button className={props.screen === "session" ? "active" : ""} onClick={() => props.onScreen("session")}>Session</button><button className={props.screen === "review" ? "active" : ""} onClick={() => props.onScreen("review")}>Review</button></div><div className="top-status">{props.busy}</div></header>
    {props.screen === "session" ? <section className="workspace">
      <TaskList tasks={props.tasks} selectedTask={props.selectedTask} onSelect={props.onSelectTask} prompt={props.taskPrompt} onPrompt={props.onTaskPrompt} onCreate={props.onCreateTask} />
      <AgentSession task={props.selectedTask} run={props.run} runs={props.runs} runFinished={props.runFinished} onPtyInput={props.onPtyInput} events={props.events} prompt={props.prompt} onPrompt={props.onPrompt} onSend={props.onSend} busy={props.busy}>
        <AgentPicker agents={props.agents} selected={props.selectedAgent} onChange={props.onAgentChange} onLaunch={props.onLaunch} runs={props.runs} run={props.run} onSelectRun={props.onSelectRun} />
      </AgentSession>
    </section> : <><nav className="review-tabs"><button className={props.drawer === "review" ? "active" : ""} onClick={() => props.onDrawer("review")}>Changes</button><button className={props.drawer === "graphs" ? "active" : ""} onClick={() => props.onDrawer("graphs")}>Graphs</button><button className={props.drawer === "activity" ? "active" : ""} onClick={() => props.onDrawer("activity")}>Activity</button><button className={props.drawer === "handoff" ? "active" : ""} onClick={() => props.onDrawer("handoff")}>Handoff</button></nav><ReviewPanel view={props.drawer} diff={props.diff} events={props.events} graph={props.graph} provenance={props.provenance} handoff={props.handoff} gitChanges={props.gitChanges} gitStatus={props.gitStatus} onGitChanged={props.onGitChanged} verification={<VerificationPanel run={props.verification} onVerify={props.onVerify} />} /></>}
    {props.error && <button className="error-toast" onClick={() => window.location.reload()}>{props.error} <span>reload</span></button>}
  </main>;
}
