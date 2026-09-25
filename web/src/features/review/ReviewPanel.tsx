import type { AgentEvent } from "../../app/api";
import type { ReactNode } from "react";

export function ReviewPanel({ view, diff, events, verification }: { view: "review" | "graphs" | "activity"; diff: string; events: AgentEvent[]; verification: ReactNode }) {
  if (view === "graphs") return <section className="drawer"><div className="drawer-title"><span>Graphs</span><small>Context map</small></div><div className="graph-placeholder"><div className="graph-node root">Task</div><div className="graph-line" /><div className="graph-node">Agent run</div><div className="graph-line" /><div className="graph-node">Changeset</div><p>Architecture and call graphs will attach here once the analysis service is migrated.</p></div></section>;
  if (view === "activity") return <section className="drawer"><div className="drawer-title"><span>Activity</span><small>{events.length} events</small></div><div className="activity-list">{events.map((event) => <div className="activity-row" key={event.id}><span className="activity-time">{new Date(event.timestamp * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}</span><strong>{event.type}</strong><span>{event.agent}</span></div>)}</div></section>;
  return <section className="drawer"><div className="drawer-title"><span>Review</span><small>Current changeset</small></div><div className="review-grid"><div><div className="subheading">DIFF</div><pre className="diff">{diff || "No changes yet."}</pre></div><div><div className="subheading">VERIFICATION</div>{verification}</div></div></section>;
}
