import type { AgentEvent, AgentRun, GitChange, GitStatus, HandoffContext, ProvenanceRecord, TaskGraph, VerificationRun } from "../../app/api";
import { CodeWorkspace } from "../code/CodeWorkspace";
import { GraphView } from "./GraphView";
import { HandoffPanel } from "./HandoffPanel";
import { ProvenancePanel } from "./ProvenancePanel";

type Props = { view: "review" | "graphs" | "activity"; taskId?: string; activeRun?: AgentRun; activeRunId?: string; taskPrompt?: string; events: AgentEvent[]; graph?: TaskGraph; provenance: ProvenanceRecord[]; handoff?: HandoffContext; gitChanges: GitChange[]; gitStatus?: GitStatus; onGitChanged: () => Promise<void>; verification?: VerificationRun; verificationRunning: boolean; onVerify: () => void; openFilePath?: string; openFileToken?: number; onOpenFile: (path: string) => void };

export function ReviewPanel(props: Props) {
  if (props.view === "graphs") return <section className="review-content"><div className="drawer-title"><span>Graphs</span><small>Task context map</small></div><GraphView graph={props.graph} /></section>;
  if (props.view === "activity") return <section className="review-content"><div className="drawer-title"><span>Activity</span><small>{props.events.length} events · task evidence</small></div>
    <section className="activity-file-links"><h2>Files mentioned by agent activity</h2>
      {props.events.filter((event) => event.type === "file.changed" && event.content.trim()).map((event) => <button key={event.id} onClick={() => props.onOpenFile(event.content.trim())}><span>{event.content.trim()}</span><small>{event.agent} · run {event.runId.slice(0, 8)} · Open diff ↗</small></button>)}
      {!props.events.some((event) => event.type === "file.changed" && event.content.trim()) && <p className="muted">No file-change events recorded for this task.</p>}
    </section>
    <ProvenancePanel records={props.provenance} />
    <details className="activity-handoff"><summary>Handoff context</summary><HandoffPanel context={props.handoff} /></details>
  </section>;
  return <CodeWorkspace taskId={props.taskId} activeRun={props.activeRun} activeRunId={props.activeRunId} taskPrompt={props.taskPrompt} events={props.events} gitChanges={props.gitChanges} gitStatus={props.gitStatus} onGitChanged={props.onGitChanged} verification={props.verification} verificationRunning={props.verificationRunning} onVerify={props.onVerify} handoff={props.handoff} openFilePath={props.openFilePath} openFileToken={props.openFileToken} />;
}
