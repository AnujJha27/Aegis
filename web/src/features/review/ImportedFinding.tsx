import { useEffect, useRef, useState } from "react";
import { api, type AgentEvent, type ReviewFinding } from "../../app/api";

export function parseImportedFinding(content: string): { file_path: string; message: string; start_line: number | null; end_line: number | null; source_origin: string | null; source_task_id: string | null; source_event_id: string | null } | null {
  try {
    const envelope = JSON.parse(content) as { historical?: unknown; text?: unknown };
    if (typeof envelope !== "object" || envelope === null || envelope.historical !== true || typeof envelope.text !== "string") return null;
    const finding = JSON.parse(envelope.text) as { kind?: unknown; file_path?: unknown; message?: unknown; start_line?: unknown; end_line?: unknown; source_origin?: unknown; source_task_id?: unknown; id?: unknown };
    if (typeof finding !== "object" || finding === null || finding.kind !== "finding" || typeof finding.file_path !== "string" || typeof finding.message !== "string") return null;
    const line = (value: unknown): number | null => (typeof value === "number" && Number.isSafeInteger(value) && value > 0 ? value : null);
    const text = (value: unknown): string | null => (typeof value === "string" && value ? value : null);
    return { file_path: finding.file_path, message: finding.message, start_line: line(finding.start_line), end_line: line(finding.end_line), source_origin: text(finding.source_origin), source_task_id: text(finding.source_task_id), source_event_id: text(finding.id) };
  } catch {
    return null;
  }
}

export function ImportedFinding({ taskId, event }: { taskId: string; event: AgentEvent }) {
  const parsed = parseImportedFinding(event.content);
  const [filePath, setFilePath] = useState("");
  const [message, setMessage] = useState("");
  const [startLine, setStartLine] = useState("");
  const [endLine, setEndLine] = useState("");
  const [pending, setPending] = useState(false);
  const [failure, setFailure] = useState("");
  const [created, setCreated] = useState<ReviewFinding | null>(null);
  const generation = useRef(0);
  const pendingRef = useRef(false);
  const mounted = useRef(true);

  useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; generation.current += 1; };
  }, []);

  useEffect(() => {
    setFilePath(parsed?.file_path ?? "");
    setMessage(parsed?.message ?? "");
    setStartLine(parsed?.start_line !== null && parsed?.start_line !== undefined ? String(parsed.start_line) : "");
    setEndLine(parsed?.end_line !== null && parsed?.end_line !== undefined ? String(parsed.end_line) : "");
    setFailure("");
    setCreated(null);
    generation.current += 1;
    pendingRef.current = false;
    setPending(false);
  }, [taskId, event.id, parsed?.file_path, parsed?.message, parsed?.start_line, parsed?.end_line]);

  if (!parsed) return null;

  const positive = (value: string): number | undefined => {
    const trimmed = value.trim();
    if (!trimmed) return undefined;
    const parsedLine = Number(trimmed);
    return Number.isSafeInteger(parsedLine) && parsedLine > 0 ? parsedLine : NaN;
  };

  const promote = async () => {
    const start = positive(startLine);
    const end = positive(endLine);
    if (Number.isNaN(start) || Number.isNaN(end)) { setFailure("Start/end lines must be positive integers."); return; }
    if (pendingRef.current) return;
    pendingRef.current = true;
    setPending(true);
    setFailure("");
    const current = ++generation.current;
    try {
      const finding = await api.createFinding(taskId, { file_path: filePath, message, start_line: start, end_line: end, source_event_id: event.id });
      if (!mounted.current || generation.current !== current) return;
      setCreated(finding);
    } catch (error) {
      if (!mounted.current || generation.current !== current) return;
      setFailure(error instanceof Error ? error.message : "Failed to promote finding.");
    } finally {
      if (mounted.current && generation.current === current) { pendingRef.current = false; setPending(false); }
    }
  };

  return <details className="imported-finding">
    <summary>Imported review finding</summary>
    <p className="imported-origin">Original source: {parsed.source_origin ?? event.agent || "system"} · task {parsed.source_task_id ?? event.taskId} · event {parsed.source_event_id ?? event.id}</p>
    <p className="imported-local-ref">Local event reference: task {event.taskId} · event {event.id}</p>
    <p className="imported-location">Source location: {parsed.file_path}{parsed.start_line !== null ? `:${parsed.start_line}${parsed.end_line !== null ? `-${parsed.end_line}` : ""}` : ""}</p>
    <label>File path<input value={filePath} onChange={(e) => setFilePath(e.target.value)} /></label>
    <label>Message<textarea value={message} onChange={(e) => setMessage(e.target.value)} rows={3} /></label>
    <label>Start line (optional)<input value={startLine} onChange={(e) => setStartLine(e.target.value)} inputMode="numeric" /></label>
    <label>End line (optional)<input value={endLine} onChange={(e) => setEndLine(e.target.value)} inputMode="numeric" /></label>
    {failure && <p className="imported-error" role="alert">{failure}</p>}
    {created && <p className="imported-created">Finding {created.id} created. Historical location — inspect current code before relying on it.</p>}
    <button type="button" onClick={promote} disabled={pending || created !== null}>{pending ? "Promoting…" : "Promote to Review"}</button>
  </details>;
}
