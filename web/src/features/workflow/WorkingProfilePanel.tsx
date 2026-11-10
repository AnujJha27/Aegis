import React, { useEffect, useRef, useState } from "react";
import {
  ProfileInstruction,
  Workflow,
  WorkingProfile as Profile,
  formatProfile,
  loadProfile,
  validateProfile,
  downloadProfile,
} from "./workingProfile";
import type { Task } from "../../app/api";
import { api } from "../../app/api";

type Props = { task: Task; onApplyWorkflow?: (workflow: Workflow) => void };

export function WorkingProfile({ task, onApplyWorkflow }: Props) {
  const [instructions, setInstructions] = useState<ProfileInstruction[]>([]);
  const [workflowsText, setWorkflowsText] = useState("{}");
  const [selected, setSelected] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [loaded, setLoaded] = useState(false);
  const [pending, setPending] = useState(false);
  const [dirty, setDirty] = useState(false);
  const [hasLegacy, setHasLegacy] = useState(false);
  const [serverHasContent, setServerHasContent] = useState(false);
  const [conflict, setConflict] = useState(false);
  const revisionRef = useRef(0);
  const generationRef = useRef(0);

  const applyProfile = (profile: Profile) => {
    setInstructions(profile.instructions.map((i) => ({ ...i })));
    setWorkflowsText(JSON.stringify({ workflows: profile.workflows }, null, 2));
    setDirty(false);
  };

  const loadFromDaemon = async () => {
    const generation = ++generationRef.current;
    setLoaded(false);
    setPending(true);
    setError(null);
    setConflict(false);
    try {
      const { revision, ...profile } = await api.workingProfile();
      if (generationRef.current !== generation) return;
      revisionRef.current = revision;
      applyProfile(profile as Profile);
      setServerHasContent(profile.instructions.length > 0 || profile.workflows.length > 0);
      try {
        setHasLegacy(!!loadProfile(task.repository));
      } catch (err) {
        setHasLegacy(false);
        setError('Browser profile could not be read; it is preserved. ' + (err instanceof Error ? err.message : String(err)));
      }
      setLoaded(true);
    } catch (err) {
      if (generationRef.current !== generation) return;
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      if (generationRef.current === generation) setPending(false);
    }
  };

  useEffect(() => {
    void loadFromDaemon();
    return () => { generationRef.current += 1; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [task.repository]);

  const markDirty = () => setDirty(true);

  const importLegacyProfile = () => {
    try {
      const legacy = loadProfile(task.repository);
      if (!legacy) { setError("No browser-saved profile found for this repository."); return; }
      applyProfile(legacy);
      setDirty(true);
      if (serverHasContent) setError("Warning: the daemon profile already has instructions/workflows. Review carefully before saving; the raw browser copy is preserved until you save.");
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    }
  };

  const update = (index: number, field: keyof ProfileInstruction, value: string) => {
    setInstructions((prev) => prev.map((item, i) => (i === index ? { ...item, [field]: value } : item)));
    markDirty();
  };
  const add = () => {
    if (instructions.length < 20) {
      setInstructions((prev) => [...prev, { text: "", source: "" }]);
      markDirty();
    }
  };
  const remove = (index: number) => {
    setInstructions((prev) => prev.filter((_, i) => i !== index));
    markDirty();
  };

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
    if (!loaded || pending) return;
    const generation = generationRef.current;
    void (async () => {
      try {
        setPending(true);
        const profile = validateProfile({ version: 1, instructions, workflows: parseWorkflows() });
        const saved = await api.saveWorkingProfile(profile, revisionRef.current);
        if (generationRef.current !== generation) return;
        revisionRef.current = saved.revision;
        setDirty(false);
        setServerHasContent(true);
        setError(null);
      } catch (err) {
        if (generationRef.current !== generation) return;
        setError(err instanceof Error ? err.message : String(err));
        setConflict(true);
      } finally {
        if (generationRef.current === generation) setPending(false);
      }
    })();
  };

  const onApply = () => {
    if (!loaded || pending) return;
    try {
      const profile = validateProfile({ version: 1, instructions, workflows: parseWorkflows() });
      const wf = profile.workflows.find((w) => w.name === selected) ?? null;
      if (wf) onApplyWorkflow?.(wf);
      setError(null);
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    }
  };

  const workflows: Workflow[] = safeWorkflows();
  const disabled = !loaded || pending;
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
        <textarea value={workflowsText} disabled={disabled} aria-label="workflow" onChange={(e) => { setWorkflowsText(e.target.value); markDirty(); }} />
        <select value={selected} disabled={disabled} onChange={(e) => setSelected(e.target.value)}>
          <option value="">(none)</option>
          {workflows.map((w) => <option key={w.name} value={w.name}>{w.name}</option>)}
        </select>
      <button disabled={disabled || !workflows.length || !selected} onClick={onApply} aria-label="apply workflow">Apply to task</button>
      </details>
      {hasLegacy && <button disabled={pending} onClick={importLegacyProfile}>Import browser profile</button>}
      <button disabled={pending} onClick={() => { if ((dirty || conflict) && !window.confirm("Discard unsaved changes and reload the daemon profile?")) return; void loadFromDaemon(); }}>Reload saved profile</button>
      <button disabled={disabled} onClick={onSave}>Save profile</button>
      <button disabled={disabled} onClick={() => exportDraft("json")}>Export JSON</button>
      <button disabled={disabled} onClick={() => exportDraft("txt")}>Export txt</button>
      <p>Stored by the local daemon; browser draft is kept until you save.</p>
      <p role="status">{dirty ? "Unsaved changes" : "Saved"}</p>
      {error && <p role="alert">{error}</p>}
    </details>
  );
}
