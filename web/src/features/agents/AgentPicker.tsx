import { useState } from "react";
import type { Agent, AgentRun } from "../../app/api";

type Props = {
  agents: Agent[];
  selected: string;
  onChange: (agent: string) => void;
  onLaunch: () => void;
  canLaunch: boolean;
  runs: AgentRun[];
  run?: AgentRun;
  runFinished: boolean;
  turnBusy: boolean;
  actionsBusy: boolean;
  onSelectRun: (runId: string) => void;
  onInterruptRun: (runId: string) => void;
  onTerminateRun: (runId: string) => void;
  onDeleteRun: (runId: string) => void;
};

export function AgentPicker({ agents, selected, onChange, onLaunch, canLaunch, runs, run, runFinished, turnBusy, actionsBusy, onSelectRun, onInterruptRun, onTerminateRun, onDeleteRun }: Props) {
  const [confirmingRunId, setConfirmingRunId] = useState("");
  const [stoppingRunId, setStoppingRunId] = useState("");
  const runAgent = agents.find((agent) => agent.name === run?.agent);
  const runActive = Boolean(run && (run.status === "starting" || run.status === "running"));
  const canInterrupt = Boolean(runActive && runAgent?.interruptible && (run?.status === "starting" || turnBusy || runAgent.interactive));
  const runState = !run ? "No active run" : run.status === "failed" ? `${run.agent} · failed` : run.status === "interrupted" ? `${run.agent} · interrupted` : run.status === "terminated" ? `${run.agent} · terminated` : run.status === "completed" ? `${run.agent} · complete` : turnBusy ? `${run.agent} · turn busy` : `${run.agent} · run active`;
  const launchLabel = !run ? "Launch agent" : selected === run.agent ? "Start another run" : `Start ${selected} run`;

  return <div className="agent-dock">
    <div className="picker-label">AGENT</div>
    <select aria-label="Agent" value={selected} onChange={(event) => onChange(event.target.value)}>{agents.map((agent) => <option key={agent.name} value={agent.name} disabled={!agent.available}>{agent.name}{agent.available ? "" : " · unavailable"}</option>)}</select>
    {runs.length > 0 && <>
      <select className="run-picker" value={run?.id ?? ""} onChange={(event) => { setConfirmingRunId(""); setStoppingRunId(""); onSelectRun(event.target.value); }} aria-label="Run history">{runs.map((item, index) => <option key={item.id} value={item.id}>{item.agent} · run {index + 1}</option>)}</select>
      <button className="delete-run-button" onClick={() => run && setConfirmingRunId(run.id)} disabled={!run || !runFinished} title={runFinished ? "Delete this run and its transcript" : "Finish the run before deleting it"} aria-label="Delete selected run">×</button>
    </>}
    <span className={`agent-state ${runActive ? "online" : ""} ${run ? turnBusy ? "busy" : run.status : "idle"}`} role="status" aria-live="polite"><i />{runState}</span>
    {runActive && runAgent?.interruptible && <button className="run-action" onClick={() => run && onInterruptRun(run.id)} disabled={!canInterrupt || actionsBusy} title={canInterrupt ? "Interrupt the current agent turn" : "No agent turn is running"}>Interrupt turn</button>}
    {runActive && <button className="run-action stop" onClick={() => run && setStoppingRunId(run.id)} disabled={actionsBusy}>Stop run</button>}
    {run && selected !== run.agent && runActive && <span className="run-switch-hint">The {run.agent} run stays active in Run history.</span>}
    <button className="launch-button" onClick={onLaunch} disabled={!canLaunch}>{launchLabel}<span>↗</span></button>
    {confirmingRunId === run?.id && run && <div className="delete-confirm" role="group" aria-label="Confirm run deletion">
      <span>Delete {run.agent} run and its transcript?</span>
      <button onClick={() => setConfirmingRunId("")}>Cancel</button>
      <button onClick={() => { onDeleteRun(run.id); setConfirmingRunId(""); }}>Delete run</button>
    </div>}
    {stoppingRunId === run?.id && run && <div className="delete-confirm" role="group" aria-label="Confirm agent stop">
      <span>Stop {run.agent}? Its transcript stays in history.</span>
      <button onClick={() => setStoppingRunId("")}>Cancel</button>
      <button onClick={() => { onTerminateRun(run.id); setStoppingRunId(""); }}>Confirm stop</button>
    </div>}
  </div>;
}
