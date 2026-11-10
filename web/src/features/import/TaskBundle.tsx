import { useEffect, useRef, useState } from "react";
import type { Task, TaskContext } from "../../app/api";
import { api } from "../../app/api";

const MAX_BUNDLE_BYTES = 1024 * 1024;
const contextFields: (keyof TaskContext)[] = ["goal", "constraints", "decisions", "attempts", "unfinished", "next_action"];

export type TaskBundle = {
  format: "aegis-task";
  version: 1;
  source_repository: string;
  prompt: string;
  context?: TaskContext;
  source_origin?: string;
  source_task_id?: string;
  messages?: unknown[];
  findings?: unknown[];
  verifications?: unknown[];
  omissions?: string[];
  working_profile?: unknown;
};

export function isTaskBundle(value: unknown): value is TaskBundle {
  if (typeof value !== "object" || value === null) return false;
  const bundle = value as Record<string, unknown>;
  if (bundle.format !== "aegis-task" || bundle.version !== 1) return false;
  if (typeof bundle.source_repository !== "string" || typeof bundle.prompt !== "string") return false;
  if (bundle.context !== undefined && (typeof bundle.context !== "object" || bundle.context === null)) return false;
  if (typeof bundle.source_origin !== "string" || bundle.source_origin.trim() === "") return false;
  if (typeof bundle.source_task_id !== "string" || bundle.source_task_id.trim() === "") return false;
  if (bundle.omissions !== undefined && (!Array.isArray(bundle.omissions) || bundle.omissions.some((v) => typeof v !== "string"))) return false;
  if (bundle.messages !== undefined && !Array.isArray(bundle.messages)) return false;
  if (bundle.findings !== undefined && !Array.isArray(bundle.findings)) return false;
  if (bundle.verifications !== undefined && !Array.isArray(bundle.verifications)) return false;
  return true;
}

export function TaskBundle({ task, repository, onImported }: { task?: Task; repository?: string; onImported?: (task: Task) => void }) {
  const [exporting, setExporting] = useState(false);
  const [importing, setImporting] = useState(false);
  const [file, setFile] = useState<File | null>(null);
  const [preview, setPreview] = useState<TaskBundle | null>(null);
  const [error, setError] = useState<string | null>(null);
  const inputRef = useRef<HTMLInputElement>(null);
  const generationRef = useRef(0);
  const pendingRef = useRef(false);

  useEffect(() => {
    generationRef.current += 1;
    setFile(null);
    setPreview(null);
    if (inputRef.current) inputRef.current.value = "";
    return () => { generationRef.current += 1; };
  }, [repository, task?.id]);

  const exportBundle = async () => {
    if (!task || exporting) return;
    const generation = generationRef.current;
    setExporting(true);
    setError(null);
    try {
      const bundle = await api.exportTaskBundle(task.id);
      if (generationRef.current !== generation) return;
      const url = URL.createObjectURL(new Blob([JSON.stringify(bundle)], { type: "application/json" }));
      const link = document.createElement("a");
      link.href = url;
      link.download = `task-${task.id}-bundle.json`;
      link.click();
      URL.revokeObjectURL(url);
      setExporting(false);
    } catch (err) {
      if (generationRef.current !== generation) return;
      setError(err instanceof Error ? err.message : "Export failed");
      setExporting(false);
    }
  };

  const loadFile = async (selected: File) => {
    generationRef.current += 1;
    const generation = generationRef.current;
    setError(null);
    if (selected.size > MAX_BUNDLE_BYTES) {
      setFile(null);
      setPreview(null);
      setError("Bundle exceeds 1 MiB limit");
      return;
    }
    try {
      const parsed: unknown = JSON.parse(await selected.text());
      if (generationRef.current !== generation) return;
      if (!isTaskBundle(parsed)) {
        setFile(null);
        setPreview(null);
        setError("Invalid bundle: expected aegis-task format version 1");
        return;
      }
      setFile(selected);
      setPreview(parsed);
    } catch {
      if (generationRef.current !== generation) return;
      setFile(null);
      setPreview(null);
      setError("Bundle is not valid JSON");
    }
  };

  const confirmImport = async () => {
    if (!preview || pendingRef.current) return;
    const generation = generationRef.current;
    pendingRef.current = true;
    setImporting(true);
    setError(null);
    try {
      const imported = await api.importTaskBundle(preview);
      if (generationRef.current !== generation) return;
      setFile(null);
      setPreview(null);
      if (inputRef.current) inputRef.current.value = "";
      onImported?.(imported);
    } catch (err) {
      if (generationRef.current !== generation) return;
      setError(err instanceof Error ? err.message : "Import failed");
    } finally {
      if (generationRef.current === generation) {
        pendingRef.current = false;
        setImporting(false);
      }
    }
  };

  return <div className="task-bundle">
    <p className="muted">Destination repository: <strong>{repository || "—"}</strong></p>
    <button onClick={exportBundle} disabled={!task || exporting}>{exporting ? "Exporting…" : "Export selected task"}</button>
    {!task && <p className="muted">Select a task to export.</p>}
    <input ref={inputRef} type="file" accept="application/json,.json" aria-label="Task bundle file" disabled={importing} onChange={(event) => { const selected = event.target.files?.[0]; if (selected) void loadFile(selected); }} />
    {preview && <div className="bundle-preview">
      <p><strong>{preview.source_repository}</strong></p>
      <p>{preview.prompt}</p>
      {contextFields.map((field) => <p key={field}><small>{field}: {preview.context?.[field] || "—"}</small></p>)}
      <p className="muted">{(Array.isArray(preview.messages) ? preview.messages : []).length} messages · {(Array.isArray(preview.findings) ? preview.findings : []).length} findings · {(Array.isArray(preview.verifications) ? preview.verifications : []).length} verifications</p>
      {preview.omissions && preview.omissions.length > 0 && <p className="muted">Omitted: {preview.omissions.join(", ")}</p>}
      <p className="muted">Verifications and findings are historical records only — never treated as current. Imported working profile is preserved for review, not auto-applied.</p>
      <button onClick={confirmImport} disabled={importing}>{importing ? "Importing…" : "Import bundle"}</button>
    </div>}
    {error && <p className="error" role="alert">{error}</p>}
  </div>;
}
