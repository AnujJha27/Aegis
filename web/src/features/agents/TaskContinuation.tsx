import { useEffect, useRef, useState } from "react";

import type { Agent, AgentRun, Task, TaskContext, ApiError } from "../../app/api";
import { api } from "../../app/api";
import { useContextDraft } from "./useContextDraft";
import { formatHandoff } from "../review/handoffText";
import { formatProfile, formatWorkflow } from "../workflow/workingProfile";
import { WorkingProfile } from "../workflow/WorkingProfilePanel";
import HandoffRecovery from "./HandoffRecovery";
import { savePendingHandoff, removePendingHandoff } from "./pendingHandoff";
import type { PendingHandoffTarget } from "./pendingHandoff";
import type { HandoffContext } from "../../app/api";

type Props = {
  task: Task;
  run?: AgentRun;
  agents: Agent[];
  sourceBusy: boolean;
  onTaskUpdated?: (task: Task) => void;
  onContinued?: (run: AgentRun, sourceRunId: string) => void;
};

const fields: [keyof TaskContext, string][] = [["goal", "Goal"], ["constraints", "Constraints"], ["decisions", "Decisions"], ["attempts", "Attempted approaches"], ["unfinished", "Unfinished work"], ["next_action", "Next action"]];
const supported = ["codex", "claude", "opencode"];
const encoder = new TextEncoder();
const validDraft = (value: string) => !value.includes("\u0000") && encoder.encode(value).length <= 4000;

export function TaskContinuation({ task, run, agents, sourceBusy, onTaskUpdated, onContinued }: Props) {
  const contextDraft = useContextDraft(task);
  const drafts = contextDraft.draft;
  const [error, setError] = useState("");
  const [dialogOpen, setDialogOpen] = useState(false);
  const [preview, setPreview] = useState("");
  const [target, setTarget] = useState("");
  const [targetModel, setTargetModel] = useState("");
  const [pending, setPending] = useState(false);
  const [recoveryEpoch, setRecoveryEpoch] = useState(0);
  const [hasPendingHandoff, setHasPendingHandoff] = useState(false);
  const genRef = useRef(0);
  const retryRef = useRef<{ target: string; model: string; key: string } | null>(null);
  const fingerprintRef = useRef<string | null | undefined>(undefined);
  const taskRef = useRef(task);
  const runRef = useRef(run);
  const draftsRef = useRef(drafts);
  const revisionRef = useRef(0);
  const targetRef = useRef("");
  const pendingRef = useRef(false);
  const hasPendingRef = useRef(false);
  const sourceBusyRef = useRef(sourceBusy);
  const dialogRef = useRef<HTMLDialogElement | null>(null);
  const focusRef = useRef<HTMLElement | null>(null);
  taskRef.current = task;
  runRef.current = run;
  draftsRef.current = drafts;
  revisionRef.current = contextDraft.revision;
  targetRef.current = target;
  sourceBusyRef.current = sourceBusy;

  const setPendingState = (value: boolean) => {
    pendingRef.current = value;
    setPending(value);
  };

  const setHasPendingState = (value: boolean) => {
    hasPendingRef.current = value;
    setHasPendingHandoff(value);
  };

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
    if (contextDraft.conflict) return false;
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
      const snapshot: TaskContext = { ...draftsRef.current };
      const saved = await api.updateTaskContext(taskId, snapshot, revisionRef.current);
      if (genRef.current !== gen || taskRef.current.id !== taskId || runRef.current?.id !== runId) return false;
      if ((saved.context_revision ?? 0) < (taskRef.current.context_revision ?? 0)) {
        contextDraft.acceptConflict(taskRef.current);
        return false;
      }
      const unchanged = contextDraft.saved(saved, snapshot);
      onTaskUpdated?.(saved);
      setError("");
      return unchanged;
    } catch (err) {
      if (genRef.current === gen) {
        const apiError = err as Partial<ApiError> | null;
        if (err instanceof Error && apiError?.status === 409 && apiError.currentTask) {
          contextDraft.acceptConflict(apiError.currentTask);
          setError("");
        } else {
          setError(err instanceof Error ? err.message : String(err));
        }
      }
      return false;
    }
  };

  const startHandoff = async () => {
    if (sourceBusyRef.current || pendingRef.current || hasPendingRef.current || !run || targets.length === 0) return;
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
      const profile = await api.workingProfile();
      if (genRef.current !== gen || taskRef.current.id !== taskId || runRef.current?.id !== sourceRunId) return;
      working = formatProfile(profile.instructions);
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
    if (pendingRef.current || hasPendingRef.current || sourceBusyRef.current) return;
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
      savePendingHandoff({ version: 1, taskId, repository: taskRef.current.repository, sourceRunId, target: target as PendingHandoffTarget, model: targetModel, prompt: preview, fingerprint: fingerprintRef.current ?? null, requestId });
      const newRun = await api.continueTask(taskId, target, sourceRunId, preview, requestId, fingerprintRef.current ?? null, targetModel || undefined);
      if (genRef.current === gen && taskRef.current.id === taskId && runRef.current?.id === sourceRunId) {
        try {
          removePendingHandoff(taskRef.current.repository, taskId, requestId);
        } catch {
          setHasPendingState(true);
          setRecoveryEpoch((epoch) => epoch + 1);
          setDialogOpen(false);
          setError("Delivered but local recovery record could not be cleared; check delivery before retrying.");
          return;
        }
        setHasPendingState(false);
        setDialogOpen(false);
        retryRef.current = null;
        onContinued?.(newRun, sourceRunId);
      }
    } catch (err) {
      if (genRef.current === gen) {
        setError(err instanceof Error ? err.message : String(err));
        setDialogOpen(false);
        setRecoveryEpoch((epoch) => epoch + 1);
      }
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
              onChange={(event) => contextDraft.edit({ ...draftsRef.current, [key]: event.target.value })}
            />
          </div>
        ))}
        <p>Context status: {contextDraft.conflict ? "Conflict" : contextDraft.storageError ? "Error" : contextDraft.dirty ? "Unsaved" : "Saved"}</p>
        <button type="button" disabled={pending || !!contextDraft.conflict} onClick={saveContextStandalone}>Save task context</button>
        {contextDraft.conflict ? (
          <div role="alert">
            <p>Conflict: task context was updated elsewhere. Saved context:</p>
            <pre>{JSON.stringify(contextDraft.conflict.context, null, 2)}</pre>
            <button type="button" onClick={() => contextDraft.resolveConflict(true)}>Use saved context</button>
            <button type="button" onClick={() => contextDraft.resolveConflict(false)}>Keep my draft</button>
          </div>
        ) : null}
        {contextDraft.storageError ? (
          <div role="alert">
            <p>{contextDraft.storageError}</p>
            <p>Edits remain in this tab only until draft storage works; save task context before reloading.</p>
            <button type="button" onClick={contextDraft.discardStored}>Discard stored draft</button>
          </div>
        ) : null}
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
          contextDraft.edit({
            ...latest,
            constraints,
            next_action: latest.next_action || wf.steps[0]?.purpose || "",
          });
        }}
      />
      <HandoffRecovery
        key={`${task.repository}/${task.id}/${recoveryEpoch}`}
        task={task}
        sourceBusy={sourceBusy}
        onContinued={onContinued}
        onPendingChange={setHasPendingState}
      />
      {error && !dialogOpen && !contextDraft.conflict && !contextDraft.storageError ? <div role="alert">{error}</div> : null}
      <button
        type="button"
        disabled={sourceBusy || !run || targets.length === 0 || pending || !!contextDraft.conflict}
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
          <select id="continue-agent" value={target} onChange={(event) => changeTarget(event.target.value)} disabled={pending}>
            {targets.map((name) => <option key={name} value={name}>{name}</option>)}
          </select>
          <label htmlFor="continue-model">Model (applies to the new run only)</label>
          <input id="continue-model" aria-label="Model for continuation" placeholder="Provider default" maxLength={200} value={targetModel} onChange={(event) => changeModel(event.target.value)} disabled={pending} />
          <label htmlFor="handoff-preview">Handoff preview</label>
          <textarea id="handoff-preview" readOnly value={preview} />
          {error && !contextDraft.conflict && !contextDraft.storageError ? <div role="alert">{error}</div> : null}
          <button type="button" disabled={pending} onClick={() => setDialogOpen(false)}>Cancel</button>
          <button type="button" disabled={pending || !target || sourceBusy} onClick={confirmContinue}>Continue task</button>
        </div>
      </dialog>
    </section>
  );
}
