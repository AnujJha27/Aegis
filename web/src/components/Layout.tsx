import type { Agent, AgentEvent, AgentRun, HandoffContext, ProvenanceRecord, Repository, Task, TaskGraph, VerificationRun } from "../app/api";
import { AgentPicker } from "../features/agents/AgentPicker";
import { ReviewPanel } from "../features/review/ReviewPanel";
import { TaskList } from "../features/tasks/TaskList";
import { AgentSession } from "../features/terminal/AgentSession";
import { VerificationPanel } from "../features/verification/VerificationPanel";

type Props = {
  repository?: Repository; tasks: Task[]; selectedTask?: Task; onSelectTask: (task: Task) => void;
  taskPrompt: string; onTaskPrompt: (value: string) => void; onCreateTask: () => void;
  agents: Agent[]; selectedAgent: string; onAgentChange: (agent: string) => void; onLaunch: () => void;
  run?: AgentRun; events: AgentEvent[]; prompt: string; onPrompt: (value: string) => void; onSend: () => void; busy: string;
  drawer?: "review" | "graphs" | "activity" | "handoff"; onDrawer: (drawer?: "review" | "graphs" | "activity" | "handoff") => void;
  diff: string; verification?: VerificationRun; handoff?: HandoffContext; graph?: TaskGraph; provenance: ProvenanceRecord[]; onVerify: () => void; error: string;
};

export function Layout(props: Props) {
  return <main className="app-shell">
    <header className="topbar"><div className="brand"><span className="brand-mark">✦</span><span>Aegis</span><small>LOCAL CONTROL PLANE</small></div><div className="repo"><span className="pulse" />{props.repository?.branch ?? "opening repository"}<span className="repo-path">{props.repository?.path ?? ""}</span></div><div className="top-status">{props.busy}</div></header>
    <section className="workspace">
      <TaskList tasks={props.tasks} selectedTask={props.selectedTask} onSelect={props.onSelectTask} prompt={props.taskPrompt} onPrompt={props.onTaskPrompt} onCreate={props.onCreateTask} />
      <AgentSession task={props.selectedTask} run={props.run} events={props.events} prompt={props.prompt} onPrompt={props.onPrompt} onSend={props.onSend} busy={props.busy}>
        <AgentPicker agents={props.agents} selected={props.selectedAgent} onChange={props.onAgentChange} onLaunch={props.onLaunch} run={props.run} />
      </AgentSession>
    </section>
    <nav className="review-nav"><button className={props.drawer === "review" ? "active" : ""} onClick={() => props.onDrawer(props.drawer === "review" ? undefined : "review")}>Review</button><button className={props.drawer === "graphs" ? "active" : ""} onClick={() => props.onDrawer(props.drawer === "graphs" ? undefined : "graphs")}>Graphs</button><button className={props.drawer === "activity" ? "active" : ""} onClick={() => props.onDrawer(props.drawer === "activity" ? undefined : "activity")}>Activity</button><button className={props.drawer === "handoff" ? "active" : ""} onClick={() => props.onDrawer(props.drawer === "handoff" ? undefined : "handoff")}>Handoff</button></nav>
    {props.drawer && <ReviewPanel view={props.drawer} diff={props.diff} events={props.events} graph={props.graph} provenance={props.provenance} handoff={props.handoff} verification={<VerificationPanel run={props.verification} onVerify={props.onVerify} />} />}
    {props.error && <button className="error-toast" onClick={() => window.location.reload()}>{props.error} <span>reload</span></button>}
  </main>;
}
