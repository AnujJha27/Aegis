import type { AgentEvent, GitChange, HandoffContext, ProvenanceRecord, TaskGraph } from "../../app/api";
import type { ReactNode } from "react";
import { GitPanel } from "../git/GitPanel";
import { GraphView } from "./GraphView";
import { HandoffPanel } from "./HandoffPanel";
import { ProvenancePanel } from "./ProvenancePanel";

export function ReviewPanel({ view, diff, events, verification, graph, provenance, handoff, gitChanges, onGitChanged }: { view: "review" | "graphs" | "activity" | "handoff"; diff: string; events: AgentEvent[]; verification: ReactNode; graph?: TaskGraph; provenance: ProvenanceRecord[]; handoff?: HandoffContext; gitChanges: GitChange[]; onGitChanged: () => Promise<void> }) {
  if (view === "graphs") return <section className="review-content"><div className="drawer-title"><span>Graphs</span><small>Task context map</small></div><GraphView graph={graph} /></section>;
  if (view === "activity") return <section className="review-content"><div className="drawer-title"><span>Activity</span><small>{events.length} events · evidence trail</small></div><ProvenancePanel records={provenance} /></section>;
  if (view === "handoff") return <section className="review-content"><div className="drawer-title"><span>Handoff</span><small>Bounded context for the next agent</small></div><HandoffPanel context={handoff} /></section>;
  return <section className="review-screen"><div className="review-grid"><div><div className="subheading">DIFF</div><pre className="diff">{diff || "No changes yet."}</pre></div><div><GitPanel changes={gitChanges} onChanged={onGitChanged} /><div className="subheading verification-heading">VERIFICATION</div>{verification}</div></div></section>;
}
