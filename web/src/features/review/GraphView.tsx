import type { TaskGraph } from "../../app/api";

const colors: Record<string, string> = { task: "#9fe870", run: "#7eb6ff", event: "#a9b6c8", file: "#e7bd79" };

export function GraphView({ graph }: { graph?: TaskGraph }) {
  if (!graph?.nodes.length) return <p className="muted">No graph data for this task yet.</p>;
  const groups = ["task", "run", "event", "file"];
  const positions = new Map(graph.nodes.map((node, index) => [node.id, { x: 115 + groups.indexOf(node.type) * 210, y: 42 + (index % 5) * 55 }]));
  return <div className="graph-view"><svg viewBox="0 0 850 330" role="img" aria-label="Task provenance graph">{graph.edges.map((edge) => { const from = positions.get(edge.from); const to = positions.get(edge.to); return from && to ? <line className="graph-link" key={`${edge.from}-${edge.to}`} x1={from.x + 72} y1={from.y + 16} x2={to.x - 8} y2={to.y + 16} /> : null; })}{graph.nodes.map((node) => { const position = positions.get(node.id)!; return <g key={node.id} transform={`translate(${position.x}, ${position.y})`}><rect width="145" height="33" rx="6" fill="#141d28" stroke={colors[node.type] ?? "#566274"} /><circle cx="14" cy="16" r="4" fill={colors[node.type] ?? "#566274"} /><text x="26" y="20" fill="#dce5f0">{node.label.slice(0, 18)}</text></g>; })}</svg><div className="graph-legend">{groups.map((group) => <span key={group}><i style={{ background: colors[group] }} />{group}</span>)}</div></div>;
}
