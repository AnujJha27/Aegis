import type { ProvenanceRecord } from "../../app/api";

export function ProvenancePanel({ records }: { records: ProvenanceRecord[] }) {
  if (!records.length) return <p className="muted">No recorded activity for this task yet.</p>;
  return <div className="provenance-list">{records.map((record) => <div className="provenance-row" key={record.event_id}><span className="provenance-source">{record.agent || "system"}</span><strong>{record.event_type}</strong><span>{record.run_id.slice(0, 8)}</span><time>{new Date(record.timestamp).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}</time><small>{record.changed_files.length ? record.changed_files.join(", ") : "file attribution pending"}</small></div>)}</div>;
}
