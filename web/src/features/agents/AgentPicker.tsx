import type { Agent, AgentRun } from "../../app/api";

export function AgentPicker({ agents, selected, onChange, onLaunch, run }: { agents: Agent[]; selected: string; onChange: (agent: string) => void; onLaunch: () => void; run?: AgentRun }) {
  return <div className="agent-dock"><div className="picker-label">AGENT</div><select value={selected} onChange={(event) => onChange(event.target.value)}>{agents.map((agent) => <option key={agent.name} value={agent.name} disabled={!agent.available}>{agent.name}{agent.available ? "" : " · unavailable"}</option>)}</select><span className={`agent-state ${run ? "online" : ""}`}><i />{run ? `${run.agent} session` : "No active run"}</span><button className="launch-button" onClick={onLaunch} disabled={Boolean(run)}>Launch agent <span>↗</span></button></div>;
}
