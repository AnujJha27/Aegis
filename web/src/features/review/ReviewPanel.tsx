import type { AgentEvent, HandoffContext, ProvenanceRecord, TaskGraph } from "../../app/api";
import type { ReactNode } from "react";
import { GraphView } from "./GraphView";
import { HandoffPanel } from "./HandoffPanel";
import { ProvenancePanel } from "./ProvenancePanel";

export function ReviewPanel({ view, diff, events, verification, graph, provenance, handoff }: { view: "review" | "graphs" | "activity" | "handoff"; diff: string; events: AgentEvent[]; verification: ReactNode; graph?: TaskGraph; provenance: ProvenanceRecord[]; handoff?: HandoffContext }) {
  if (view === "graphs") return <section className="drawer"><div className="drawer-title"><span>Graphs</span><small>Task context map</small></div><GraphView graph={graph} /></section>;
  if (view === "activity") return <section className="drawer"><div className="drawer-title"><span>Activity</span><small>{events.length} events · evidence trail</small></div><ProvenancePanel records={provenance} /></section>;
  if (view === "handoff") return <section className="drawer"><div className="drawer-title"><span>Handoff</span><small>Bounded context for the next agent</small></div><HandoffPanel context={handoff} /></section>;
  return <section className="drawer"><div className="drawer-title"><span>Review</span><small>Current changeset</small></div><div className="review-grid"><div><div className="subheading">DIFF</div><pre className="diff">{diff || "No changes yet."}</pre></div><div><div className="subheading">VERIFICATION</div>{verification}</div></div></section>;
}
