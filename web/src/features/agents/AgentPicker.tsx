import { useState } from "react";
import type { Agent, AgentRun } from "../../app/api";

export function AgentPicker({ agents, selected, onChange, onLaunch, runs, run, runFinished, onSelectRun, onDeleteRun }: { agents: Agent[]; selected: string; onChange: (agent: string) => void; onLaunch: () => void; runs: AgentRun[]; run?: AgentRun; runFinished: boolean; onSelectRun: (runId: string) => void; onDeleteRun: (runId: string) => void }) {
  const [confirmingRunId, setConfirmingRunId] = useState("");
  return <div className="agent-dock">
    <div className="picker-label">AGENT</div>
      <select aria-label="Agent" value={selected} onChange={(event) => onChange(event.target.value)}>{agents.map((agent) => <option key={agent.name} value={agent.name} disabled={!agent.available}>{agent.name}{agent.available ? "" : " · unavailable"}</option>)}</select>
    {runs.length > 0 && <>
      <select className="run-picker" value={run?.id ?? ""} onChange={(event) => { setConfirmingRunId(""); onSelectRun(event.target.value); }} aria-label="Run history">{runs.map((item, index) => <option key={item.id} value={item.id}>{item.agent} · run {index + 1}</option>)}</select>
      <button className="delete-run-button" onClick={() => run && setConfirmingRunId(run.id)} disabled={!run || !runFinished} title={runFinished ? "Delete this run and its transcript" : "Finish the run before deleting it"} aria-label="Delete selected run">×</button>
    </>}
    <span className={`agent-state ${run ? "online" : ""}`}><i />{run ? `${run.agent} session` : "No active run"}</span>
    <button className="launch-button" onClick={onLaunch}>{run ? "Switch agent" : "Launch agent"} <span>↗</span></button>
    {confirmingRunId === run?.id && run && <div className="delete-confirm" role="group" aria-label="Confirm run deletion">
      <span>Delete {run.agent} run and its transcript?</span>
      <button onClick={() => setConfirmingRunId("")}>Cancel</button>
      <button onClick={() => { onDeleteRun(run.id); setConfirmingRunId(""); }}>Delete run</button>
    </div>}
  </div>;
}
