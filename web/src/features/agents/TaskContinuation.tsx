import { useEffect, useRef, useState } from "react";
import type { Agent, AgentRun, Task, TaskContext } from "../../app/api";
import { api } from "../../app/api";
import { formatHandoff } from "../review/handoffText";
import { loadProfile, formatProfile, formatWorkflow } from "../workflow/workingProfile";
import { WorkingProfile } from "../workflow/WorkingProfilePanel";
import type { HandoffContext } from "../../app/api";

type Props = {
  task: Task;
  run?: AgentRun;
  agents: Agent[];
  sourceBusy: boolean;
  onTaskUpdated?: (task: Task) => void;
  onContinued?: (run: AgentRun, sourceRunId: string) => void;
};

const emptyContext: TaskContext = { goal: "", constraints: "", decisions: "", attempts: "", unfinished: "", next_action: "" };
const fields: [keyof TaskContext, string][] = [["goal", "Goal"], ["constraints", "Constraints"], ["decisions", "Decisions"], ["attempts", "Attempted approaches"], ["unfinished", "Unfinished work"], ["next_action", "Next action"]];
const supported = ["codex", "claude", "opencode"];
const encoder = new TextEncoder();
const validDraft = (value: string) => !value.includes("\u0000") && encoder.encode(value).length <= 4000;

export function TaskContinuation({ task, run, agents, sourceBusy, onTaskUpdated, onContinued }: Props) {
  const [drafts, setDrafts] = useState<TaskContext>(() => ({ ...emptyContext, ...(task.context ?? {}) }));
  const [error, setError] = useState("");
  const [dialogOpen, setDialogOpen] = useState(false);
  const [preview, setPreview] = useState("");
  const [target, setTarget] = useState("");
  const [targetModel, setTargetModel] = useState("");
  const [pending, setPending] = useState(false);
  const genRef = useRef(0);
  const retryRef = useRef<{ target: string; model: string; key: string } | null>(null);
  const fingerprintRef = useRef<string | null | undefined>(undefined);
  const taskRef = useRef(task);
  const runRef = useRef(run);
  const draftsRef = useRef(drafts);
  const targetRef = useRef("");
  const pendingRef = useRef(false);
  const sourceBusyRef = useRef(sourceBusy);
  const dialogRef = useRef<HTMLDialogElement | null>(null);
  const focusRef = useRef<HTMLElement | null>(null);
  taskRef.current = task;
  runRef.current = run;
  draftsRef.current = drafts;
  targetRef.current = target;
  sourceBusyRef.current = sourceBusy;

  const setPendingState = (value: boolean) => {
    pendingRef.current = value;
    setPending(value);
  };

  useEffect(() => {
    setDrafts({ ...emptyContext, ...(task.context ?? {}) });
  }, [task.id]);

  useEffect(() => {
    genRef.current += 1;
    setError("");
    setDialogOpen(false);
    setPendingState(false);
    return () => {
      genRef.current += 1;
    };
  }, [task.id, run?.id]);

  useEffect(() => {
    const dialog = dialogRef.current;
    if (!dialog) return;
    if (dialogOpen) {
      focusRef.current = document.activeElement as HTMLElement | null;
      if (typeof dialog.showModal === "function") dialog.showModal();
      else dialog.setAttribute("open", "");
    } else if (dialog.hasAttribute("open")) {
      if (typeof dialog.close === "function") dialog.close();
      else dialog.removeAttribute("open");
      focusRef.current?.focus();
    }
  }, [dialogOpen]);

  const targets = agents.filter((a) => a.available && supported.includes(a.name) && a.name !== run?.agent).map((a) => a.name);

  const saveContext = async (): Promise<boolean> => {
    for (const [key] of fields) {
      if (!validDraft(draftsRef.current[key])) {
        setError(`${key}: must be at most 4000 UTF-8 bytes and contain no NUL characters`);
        return false;
      }
    }
    const gen = genRef.current;
    const taskId = taskRef.current.id;
    const runId = runRef.current?.id;
    try {
      const saved = await api.updateTaskContext(taskId, draftsRef.current);
      if (genRef.current !== gen || taskRef.current.id !== taskId || runRef.current?.id !== runId) return false;
      onTaskUpdated?.(saved);
      setError("");
      return true;
    } catch (err) {
      if (genRef.current === gen) setError(err instanceof Error ? err.message : String(err));
      return false;
    }
  };

  const startHandoff = async () => {
    if (sourceBusyRef.current || pendingRef.current || !run || targets.length === 0) return;
    const gen = genRef.current;
    const taskId = taskRef.current.id;
    const sourceRunId = runRef.current?.id;
    if (!sourceRunId) return;
    setPendingState(true);
    const saved = await saveContext();
    if (!saved) {
      if (genRef.current === gen) setPendingState(false);
      return;
    }
    let working = "";
    try {
      const profile = loadProfile(taskRef.current.repository ?? "");
      if (profile) working = formatProfile(profile.instructions);
    } catch (err) {
      if (genRef.current === gen) setError(err instanceof Error ? err.message : String(err));
      setPendingState(false);
      return;
    }
    try {
      const handoff: HandoffContext = await api.handoff(taskId);
      if (genRef.current !== gen || taskRef.current.id !== taskId || runRef.current?.id !== sourceRunId) return;
      setPreview(formatHandoff(handoff, runRef.current, working));
      setTarget(targets[0]);
      setTargetModel("");
      fingerprintRef.current = handoff.repository_fingerprint ?? null;
      retryRef.current = { target: targets[0], model: "", key: crypto.randomUUID() };
      setError("");
      setDialogOpen(true);
    } catch (err) {
      if (genRef.current === gen) setError(err instanceof Error ? err.message : String(err));
    } finally {
      if (genRef.current === gen) setPendingState(false);
    }
  };

  const changeTarget = (name: string) => {
    setTarget(name);
    setTargetModel("");
    if (!retryRef.current || retryRef.current.target !== name || retryRef.current.model !== "") {
      retryRef.current = { target: name, model: "", key: crypto.randomUUID() };
    }
  };

  const changeModel = (value: string) => {
    setTargetModel(value);
    if (!retryRef.current || retryRef.current.model !== value) {
      retryRef.current = { target: targetRef.current, model: value, key: crypto.randomUUID() };
    }
  };

  const confirmContinue = async () => {
    if (pendingRef.current || sourceBusyRef.current) return;
    const gen = genRef.current;
    const taskId = taskRef.current.id;
    const sourceRunId = runRef.current?.id;
    if (!sourceRunId) return;
    if (!target || !targets.includes(target)) {
      setError("Selected target is no longer available");
      return;
    }
    if (targetModel !== "" && (targetModel.length > 200 || !/^[A-Za-z0-9][A-Za-z0-9_./:-]*$/.test(targetModel))) {
      setError("Model must be ASCII, start with a letter or digit, contain only letters, digits, _ . / : - and be at most 200 characters");
      return;
    }
    setPendingState(true);
    try {
      const requestId = retryRef.current?.target === target && retryRef.current?.model === targetModel ? retryRef.current.key : crypto.randomUUID();
      retryRef.current = { target, model: targetModel, key: requestId };
      const newRun = await api.continueTask(taskId, target, sourceRunId, preview, requestId, fingerprintRef.current, targetModel || undefined);
      if (genRef.current === gen && taskRef.current.id === taskId && runRef.current?.id === sourceRunId) {
        onContinued?.(newRun, sourceRunId);
        setDialogOpen(false);
        retryRef.current = null;
      }
    } catch (err) {
      if (genRef.current === gen) setError(err instanceof Error ? err.message : String(err));
    } finally {
      if (genRef.current === gen) setPendingState(false);
    }
  };

  const saveContextStandalone = async () => {
    if (pendingRef.current) return;
    const gen = genRef.current;
    setPendingState(true);
    try {
      await saveContext();
    } finally {
      if (genRef.current === gen) setPendingState(false);
    }
  };

  return (
    <section className="task-continuation">
      <details>
        <summary>Task context</summary>
        {fields.map(([key, label]) => (
          <div key={key}>
            <label htmlFor={`task-context-${key}`}>{label}</label>
            <textarea
              id={`task-context-${key}`}
              value={drafts[key]}
              onChange={(event) => setDrafts((prev) => ({ ...prev, [key]: event.target.value }))}
            />
          </div>
        ))}
        <button type="button" disabled={pending} onClick={saveContextStandalone}>Save task context</button>
      </details>
      <WorkingProfile
        task={task}
        onApplyWorkflow={(wf) => {
          const latest = draftsRef.current;
          const formatted = formatWorkflow(wf);
          const constraints = latest.constraints ? `${latest.constraints}\n${formatted}` : formatted;
          if (encoder.encode(constraints).length > 4000) {
            setError("Applying workflow would exceed 4000 UTF-8 bytes of constraints; shorten it first");
            return;
          }
          setDrafts({
            ...latest,
            constraints,
            next_action: latest.next_action || wf.steps[0]?.purpose || "",
          });
        }}
      />
      {error && !dialogOpen ? <div role="alert">{error}</div> : null}
      <button
        type="button"
        disabled={sourceBusy || !run || targets.length === 0 || pending}
        onClick={startHandoff}
      >
        Continue with another agent
      </button>
      <dialog
        ref={dialogRef}
        aria-label="Continue with another agent"
        role="dialog"
        onCancel={(event) => { event.preventDefault(); if (!pendingRef.current) setDialogOpen(false); }}
      >
        <div>
          <label htmlFor="continue-agent">Continue with</label>
          <select id="continue-agent" value={target} onChange={(event) => changeTarget(event.target.value)}>
            {targets.map((name) => <option key={name} value={name}>{name}</option>)}
          </select>
          <label htmlFor="continue-model">Model (applies to the new run only)</label>
          <input id="continue-model" aria-label="Model for continuation" placeholder="Provider default" maxLength={200} value={targetModel} onChange={(event) => changeModel(event.target.value)} disabled={pending} />
          <label htmlFor="handoff-preview">Handoff preview</label>
          <textarea id="handoff-preview" readOnly value={preview} />
          {error ? <div role="alert">{error}</div> : null}
          <button type="button" disabled={pending} onClick={() => setDialogOpen(false)}>Cancel</button>
          <button type="button" disabled={pending || !target || sourceBusy} onClick={confirmContinue}>Continue task</button>
        </div>
      </dialog>
    </section>
  );
}
