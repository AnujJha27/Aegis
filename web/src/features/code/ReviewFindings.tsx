import { useEffect, useRef, useState, type FormEvent } from "react";
import { api, type AgentRun, type ReviewFinding } from "../../app/api";

const MAX_PROMPT_BYTES = 60000;
const encoder = new TextEncoder();

function anchorLabel(finding: ReviewFinding): string {
  if (finding.freshness === "stale") return "Earlier code";
  if (finding.freshness === "current") return "Current code";
  return "Code version unknown";
}

function locationLabel(finding: ReviewFinding): string {
  if (!finding.start_line) return "file-level";
  if (finding.end_line && finding.end_line !== finding.start_line) return `lines ${finding.start_line}-${finding.end_line}`;
  return `line ${finding.start_line}`;
}

function revisionPrompt(findings: ReviewFinding[]): string {
  return [
    "Review revision request",
    "",
    "Address the selected review findings below. Findings were recorded against a specific code version; treat them as historical evidence and verify each location against the current working tree before editing.",
    "",
    ...findings.map((finding) => `- ${finding.file_path} · ${locationLabel(finding)} · ${anchorLabel(finding)} — ${finding.message} (finding ${finding.id})`),
  ].join("\n");
}

function sendEligibility(run?: AgentRun): { eligible: boolean; reason: string } {
  if (!run) return { eligible: false, reason: "No agent run to receive a revision request." };
  if (run.agent !== "codex") return { eligible: false, reason: `Agent ${run.agent} cannot receive interactive revision requests; only codex can.` };
  if (run.status !== "running") return { eligible: false, reason: `Agent run is ${run.status}; a revision request needs a running run.` };
  if (run.turnStatus === "turn.started") return { eligible: false, reason: "Agent is already processing a turn; wait for it to finish before requesting a revision." };
  return { eligible: true, reason: "" };
}

export function ReviewFindings({ taskId, activeRun, activeRunId, filePath }: { taskId?: string; activeRun?: AgentRun; activeRunId?: string; filePath?: string }) {
  const [findings, setFindings] = useState<ReviewFinding[]>([]);
  const [message, setMessage] = useState("");
  const [start, setStart] = useState("");
  const [end, setEnd] = useState("");
  const [loading, setLoading] = useState(false);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState("");
  const [selected, setSelected] = useState<string[]>([]);
  const [preview, setPreview] = useState<{ prompt: string; runId: string; taskId: string } | null>(null);
  const [sendError, setSendError] = useState("");
  const [sending, setSending] = useState(false);
  const generationRef = useRef(0);
  const taskRef = useRef(taskId);
  const fileRef = useRef(filePath);
  taskRef.current = taskId;
  fileRef.current = filePath;

  useEffect(() => {
    let cancelled = false;
    generationRef.current += 1;
    setFindings([]);
    setSelected([]);
    setPreview(null);
    setSendError("");
    setError("");
    setStart("");
    setEnd("");
    setSaving(false);
    setSending(false);
    if (!taskId) { setLoading(false); return; }
    setLoading(true);
    const load = () => {
      void api.findings(taskId).then((items) => {
        if (!cancelled && taskRef.current === taskId) setFindings(items);
      }).catch((reason: Error) => {
        if (!cancelled && taskRef.current === taskId) setError(reason.message);
      }).finally(() => {
        if (!cancelled && taskRef.current === taskId) setLoading(false);
      });
    };
    load();
    const onResync = () => { if (taskRef.current === taskId) load(); };
    window.addEventListener("aegis:repository", onResync);
    window.addEventListener("aegis:resync", onResync);
    return () => {
      cancelled = true;
      generationRef.current += 1;
      window.removeEventListener("aegis:repository", onResync);
      window.removeEventListener("aegis:resync", onResync);
    };
  }, [taskId]);

  useEffect(() => {
    setPreview(null);
    setSendError("");
  }, [activeRunId]);

  const sorted = [...findings].sort((a, b) => {
    const aOpen = a.status === "open" ? 0 : 1;
    const bOpen = b.status === "open" ? 0 : 1;
    if (aOpen !== bOpen) return aOpen - bOpen;
    return b.created_at - a.created_at;
  });
  const visible = sorted.filter((finding) => finding.status === "open" || finding.file_path === filePath);
  const selectedFindings = sorted.filter((finding) => finding.status === "open" && selected.includes(finding.id));
  const promptText = revisionPrompt(selectedFindings);
  const promptBytes = encoder.encode(promptText).length;
  const eligibility = sendEligibility(activeRun);
  const previewStale = !!preview && (!activeRun || activeRun.id !== preview.runId || preview.taskId !== taskId);
  const canSend = !!preview && eligibility.eligible && !previewStale;

  async function add(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!taskId || !filePath || !message.trim()) return;
    const startLine = start ? Number(start) : undefined;
    const endLine = end ? Number(end) : undefined;
    if ((endLine !== undefined && startLine === undefined) ||
        (startLine !== undefined && (!Number.isSafeInteger(startLine) || startLine < 1 || startLine > 2147483647)) ||
        (endLine !== undefined && (!Number.isSafeInteger(endLine) || endLine < 1 || endLine > 2147483647)) ||
        (startLine !== undefined && endLine !== undefined && endLine < startLine)) {
      setError("Enter a valid positive line range.");
      return;
    }
    const gen = generationRef.current;
    const taskAtStart = taskId;
    const fileAtStart = filePath;
    setSaving(true);
    setError("");
    try {
      const created = await api.createFinding(taskId, {
        file_path: filePath, run_id: activeRunId, start_line: startLine, end_line: endLine,
        message: message.trim(),
      });
      if (generationRef.current !== gen || taskRef.current !== taskAtStart || fileRef.current !== fileAtStart) return;
      setFindings((current) => [created, ...current]);
      setMessage("");
      setStart("");
      setEnd("");
    } catch (reason) {
      if (generationRef.current !== gen || taskRef.current !== taskAtStart || fileRef.current !== fileAtStart) return;
      setError(reason instanceof Error ? reason.message : "Could not add finding");
    } finally {
      if (generationRef.current === gen && taskRef.current === taskAtStart) setSaving(false);
    }
  }

  async function toggleStatus(finding: ReviewFinding) {
    const status = finding.status === "open" ? "resolved" : "open";
    const gen = generationRef.current;
    const taskAtStart = taskId;
    setError("");
    try {
      await api.updateFindingStatus(finding.id, status);
      if (generationRef.current !== gen || taskRef.current !== taskAtStart) return;
      setFindings((current) => current.map((item) => item.id === finding.id ? { ...item, status } : item));
    } catch (reason) {
      if (generationRef.current !== gen || taskRef.current !== taskAtStart) return;
      setError(reason instanceof Error ? reason.message : "Could not update finding");
    }
  }

  function toggleSelected(id: string) {
    setSelected((current) => current.includes(id) ? current.filter((item) => item !== id) : [...current, id]);
  }

  function openPreview() {
    setSendError("");
    if (encoder.encode(revisionPrompt(selectedFindings)).length > MAX_PROMPT_BYTES) {
      setError(`Revision request exceeds ${MAX_PROMPT_BYTES} UTF-8 bytes; deselect findings instead of truncating.`);
      return;
    }
    setPreview({ prompt: promptText, runId: activeRun?.id ?? "", taskId: taskId ?? "" });
  }

  async function sendRevision() {
    if (!preview || sending || !canSend) return;
    const gen = generationRef.current;
    const taskAtStart = taskId;
    const runAtStart = preview.runId;
    setSending(true);
    setSendError("");
    try {
      await api.send(runAtStart, preview.prompt);
      if (generationRef.current !== gen || taskRef.current !== taskAtStart) return;
      setSelected([]);
      setPreview(null);
    } catch (reason) {
      if (generationRef.current !== gen || taskRef.current !== taskAtStart) return;
      setSendError(reason instanceof Error ? reason.message : "Could not send revision request");
    } finally {
      if (generationRef.current === gen && taskRef.current === taskAtStart) setSending(false);
    }
  }

  return <section className="review-side-section findings-section" aria-label="Review findings">
    <span className="subheading">FINDINGS · {filePath ? visible.length : findings.length}</span>
    {filePath && taskId && <form className="finding-form" onSubmit={(event) => void add(event)}>
      <label>Finding note<textarea aria-label="Finding note" rows={3} maxLength={8000} value={message} onChange={(event) => setMessage(event.target.value)} placeholder="Record an issue or review note…" required /></label>
      <div className="finding-lines"><label>From line<input aria-label="Start line" type="number" min="1" max="2147483647" value={start} onChange={(event) => setStart(event.target.value)} /></label><label>To line<input aria-label="End line" type="number" min="1" max="2147483647" value={end} onChange={(event) => setEnd(event.target.value)} /></label></div>
      <button type="submit" disabled={saving || !message.trim()}>{saving ? "Adding…" : "Add finding"}</button>
    </form>}
    {!filePath && <p className="muted">Select a file to add or inspect its findings.</p>}
    {!taskId && <p className="muted">Select a task to save review findings.</p>}
    {loading && <p className="muted">Loading findings…</p>}
    {error && <p className="file-tree-error" role="alert">{error}</p>}
    {taskId && <div className="revision-request">
      {visible.some((finding) => finding.status === "open") && <>
        <button type="button" disabled={!selectedFindings.length || !eligibility.eligible} onClick={openPreview}>Review revision request</button>
        {!eligibility.eligible && <small role="note">{eligibility.reason}</small>}
      </>}
      {preview && <details className="revision-preview" open>
        <summary>Review revision request preview (read-only)</summary>
        <div role="dialog" aria-label="Revision request preview">
          <pre className="revision-prompt" contentEditable={false}>{preview.prompt}</pre>
          <small>{encoder.encode(preview.prompt).length} / {MAX_PROMPT_BYTES} UTF-8 bytes · nothing is sent until you confirm.</small>
          {sendError && <p className="file-tree-error" role="alert">{sendError}</p>}
          <button type="button" onClick={() => setPreview(null)} disabled={sending}>Cancel</button>
          <button type="button" aria-label="Send for revision" disabled={sending || !canSend} onClick={() => void sendRevision()}>{sending ? "Sending…" : "Send for revision"}</button>
          {previewStale && <small role="note">The active run or task changed; reopen the preview to send.</small>}
        </div>
      </details>}
    </div>}
    <ul className="finding-list">{visible.map((finding) => <li key={finding.id} className={finding.status}>
      <div className="finding-row">
        {finding.status === "open" && <input type="checkbox" aria-label={`Select finding ${finding.id}`} checked={selected.includes(finding.id)} onChange={() => toggleSelected(finding.id)} />}
        <p>{finding.message}</p>
      </div>
      <small>{(finding.status === "open" && (!filePath || finding.file_path !== filePath)) && <code className="side-file-path">{finding.file_path} · </code>}{finding.start_line ? `Line ${finding.start_line}${finding.end_line && finding.end_line !== finding.start_line ? `–${finding.end_line}` : ""}` : "File-level note"}{finding.status === "open" ? ` · ${anchorLabel(finding)}` : ""}{finding.run_id ? ` · run ${finding.run_id.slice(0, 8)}` : ""} · {finding.status}</small>
      <button aria-label={finding.status === "open" ? "Resolve finding" : "Reopen finding"} onClick={() => void toggleStatus(finding)}>{finding.status === "open" ? "Resolve" : "Reopen"}</button>
    </li>)}</ul>
  </section>;
}
