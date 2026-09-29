import { useEffect, useState, type FormEvent } from "react";
import { api, type ReviewFinding } from "../../app/api";

export function ReviewFindings({ taskId, activeRunId, filePath }: { taskId?: string; activeRunId?: string; filePath?: string }) {
  const [findings, setFindings] = useState<ReviewFinding[]>([]);
  const [message, setMessage] = useState("");
  const [start, setStart] = useState("");
  const [end, setEnd] = useState("");
  const [loading, setLoading] = useState(false);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState("");

  useEffect(() => {
    let cancelled = false;
    setFindings([]);
    setMessage("");
    setStart("");
    setEnd("");
    setError("");
    if (!taskId) { setLoading(false); return; }
    setLoading(true);
    void api.findings(taskId).then((items) => {
      if (!cancelled) setFindings(items);
    }).catch((reason: Error) => {
      if (!cancelled) setError(reason.message);
    }).finally(() => {
      if (!cancelled) setLoading(false);
    });
    return () => { cancelled = true; };
  }, [taskId]);

  const visible = findings.filter((finding) => finding.file_path === filePath);

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
    setSaving(true);
    setError("");
    try {
      const created = await api.createFinding(taskId, {
        file_path: filePath, run_id: activeRunId, start_line: startLine, end_line: endLine,
        message: message.trim(),
      });
      setFindings((current) => [created, ...current]);
      setMessage("");
      setStart("");
      setEnd("");
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Could not add finding");
    } finally {
      setSaving(false);
    }
  }

  async function toggleStatus(finding: ReviewFinding) {
    const status = finding.status === "open" ? "resolved" : "open";
    setError("");
    try {
      await api.updateFindingStatus(finding.id, status);
      setFindings((current) => current.map((item) => item.id === finding.id ? { ...item, status } : item));
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Could not update finding");
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
    <ul className="finding-list">{visible.map((finding) => <li key={finding.id} className={finding.status}>
      <p>{finding.message}</p><small>{finding.start_line ? `Line ${finding.start_line}${finding.end_line && finding.end_line !== finding.start_line ? `–${finding.end_line}` : ""}` : "File-level note"}{finding.run_id ? ` · run ${finding.run_id.slice(0, 8)}` : ""} · {finding.status}</small>
      <button aria-label={finding.status === "open" ? "Resolve finding" : "Reopen finding"} onClick={() => void toggleStatus(finding)}>{finding.status === "open" ? "Resolve" : "Reopen"}</button>
    </li>)}</ul>
  </section>;
}
