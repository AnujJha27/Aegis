import React, { useEffect, useRef, useState } from "react";
import {
  WorkingProfile as Profile,
  ProfileInstruction,
  Workflow,
  formatProfile,
  formatWorkflow,
  loadProfile,
  saveProfile,
  validateProfile,
  downloadProfile,
} from "./workingProfile";
import type { Task } from "../../app/api";

type Props = { task: Task; onApplyWorkflow?: (workflow: Workflow) => void };

export function WorkingProfile({ task, onApplyWorkflow }: Props) {
  const [instructions, setInstructions] = useState<ProfileInstruction[]>([]);
  const [workflowsText, setWorkflowsText] = useState("{}");
  const [selected, setSelected] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [loaded, setLoaded] = useState(false);
  useEffect(() => {
    try {
      const p = loadProfile(task.repository);
      setInstructions(p ? p.instructions.map((i) => ({ ...i })) : []);
      setWorkflowsText(JSON.stringify(p ? { workflows: p.workflows } : { workflows: [] }, null, 2));
      setError(null);
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    }
    setLoaded(true);
  }, [task.repository]);

  const update = (index: number, field: keyof ProfileInstruction, value: string) =>
    setInstructions((prev) => prev.map((item, i) => (i === index ? { ...item, [field]: value } : item)));
  const add = () => instructions.length < 20 && setInstructions((prev) => [...prev, { text: "", source: "" }]);
  const remove = (index: number) => setInstructions((prev) => prev.filter((_, i) => i !== index));

  const parseWorkflowsText = (): unknown[] => {
    const parsed: unknown = JSON.parse(workflowsText);
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) throw new Error("Workflows JSON must be an object");
    const workflows = (parsed as { workflows?: unknown }).workflows;
    if (!Array.isArray(workflows)) throw new Error("Workflows must be an array");
    return workflows;
  };

  const parseWorkflows = (): Workflow[] =>
    parseWorkflowsText().map((entry) => validateProfile({ version: 1, instructions: [], workflows: [entry] }).workflows[0]);

  const safeWorkflows = (): Workflow[] => { try { return parseWorkflows(); } catch { return []; } };

  const exportDraft = (kind: "json" | "txt") => {
    try {
      const profile = validateProfile({ version: 1, instructions, workflows: parseWorkflows() });
      if (kind === "json") downloadProfile(JSON.stringify(profile), "aegis-profile.json", "application/json");
      else downloadProfile(formatProfile(profile.instructions), "aegis-profile.txt", "text/plain");
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    }
  };

  const onSave = () => {
    if (!loaded) return;
    try {
      const profile = validateProfile({ version: 1, instructions, workflows: parseWorkflows() });
      saveProfile(task.repository, profile);
      setError(null);
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    }
  };

  const onApply = () => {
    if (!loaded) return;
    try {
      const profile = validateProfile({ version: 1, instructions, workflows: parseWorkflows() });
      saveProfile(task.repository, profile);
      const wf = profile.workflows.find((w) => w.name === selected) ?? null;
      if (wf) onApplyWorkflow?.(wf);
      setError(null);
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    }
  };

  const workflows: Workflow[] = safeWorkflows();
  const disabled = !loaded;
  return (
    <details className="working-profile">
      <summary>Working style</summary>
      {instructions.map((inst, i) => (
        <div key={i}>
          <input value={inst.text} disabled={disabled} aria-label="instruction" onChange={(e) => update(i, "text", e.target.value)} placeholder="instruction" />
          <input value={inst.source} disabled={disabled} aria-label="source" onChange={(e) => update(i, "source", e.target.value)} placeholder="source" />
          <button disabled={disabled} onClick={() => remove(i)}>Remove</button>
        </div>
      ))}
      <button disabled={disabled || instructions.length >= 20} onClick={add}>Add instruction</button>
      <details>
        <summary>Reusable workflows</summary>
        <textarea value={workflowsText} disabled={disabled} aria-label="workflow" onChange={(e) => setWorkflowsText(e.target.value)} />
        <select value={selected} disabled={disabled} onChange={(e) => setSelected(e.target.value)}>
          <option value="">(none)</option>
          {workflows.map((w) => <option key={w.name} value={w.name}>{w.name}</option>)}
        </select>
      <button disabled={disabled || !workflows.length || !selected} onClick={onApply} aria-label="apply workflow">Apply to task</button>
      </details>
      <button disabled={disabled} onClick={onSave}>Save profile</button>
      <button disabled={disabled} onClick={() => exportDraft("json")}>Export JSON</button>
      <button disabled={disabled} onClick={() => exportDraft("txt")}>Export txt</button>
      <p>Saved in this browser's local storage only.</p>
      {error && <p role="alert">{error}</p>}
    </details>
  );
}
