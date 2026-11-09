import { useEffect, useRef, useState } from "react";
import { api, type Task, type TaskContext } from "../../app/api";
import { evidenceCards, parseCodexLog, type CodexLog, type EvidenceCard } from "./codexLog";
import { loadProfile, saveProfile } from "../workflow/workingProfile";

const bytes = (s: string) => new TextEncoder().encode(s).length;
const truncate = (s: string, max: number) => {
  if (bytes(s) <= max) return s;
  let out = "", used = 0;
  for (const ch of s) { const b = bytes(ch); if (used + b > max) break; out += ch; used += b; }
  return out;
};
const emptyContext: TaskContext = { goal: "", constraints: "", decisions: "", attempts: "", unfinished: "", next_action: "" };
const contextFieldList = ["goal", "constraints", "decisions", "attempts", "unfinished", "next_action"] as const;

export function ImportConversation({ repository, onImported }: { repository?: string; onImported?: (task: Task) => void }) {
  const [prompt, setPrompt] = useState("");
  const [context, setContext] = useState<TaskContext>(emptyContext);
  const [log, setLog] = useState<CodexLog | null>(null);
  const [sourceName, setSourceName] = useState("");
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [warnings, setWarnings] = useState<string[]>([]);
  const [error, setError] = useState("");
  const [pending, setPending] = useState(false);
  const [retained, setRetained] = useState("");
  const generation = useRef(0);

  useEffect(() => () => { generation.current++; }, []);

  const key = (m: { source_name: string; source_line: number }) => `${m.source_name}:${m.source_line}`;
  const cards: EvidenceCard[] = log ? evidenceCards(log) : [];

  async function load(file: File) {
    setError("");
    const gen = ++generation.current;
    if (file.size > 8 * 1024 * 1024) { setError("File exceeds 8 MiB limit"); return; }
    setPending(true);
    try {
      const parsed = parseCodexLog(await file.text(), file.name);
      if (gen !== generation.current) return;
      setLog(parsed);
      setSourceName(file.name);
      setSelected(new Set(parsed.messages.map(key)));
      setWarnings(parsed.warnings);
      const firstUser = parsed.messages.find((m) => m.role === "user")?.text ?? "";
      setPrompt(firstUser);
      setContext({ ...emptyContext, goal: truncate(firstUser, 4000) });
    } catch (reason) {
      if (gen !== generation.current) return;
      setError(reason instanceof Error ? reason.message : "Could not parse file");
      setLog(null); setWarnings([]); setSelected(new Set());
    } finally { if (gen === generation.current) setPending(false); }
  }

  function toggleCard(card: EvidenceCard) {
    setError("");
    const k = `${card.source_name}:${card.source_line}`;
    const next = new Set(selected);
    if (next.has(k)) next.delete(k); else next.add(k);
    setSelected(next);
  }

  function retainInstruction(card: EvidenceCard) {
    setRetained("");
    setError("");
    if (!repository) return;
    const source = `Codex ${log?.sessionId ?? "unknown"} ${card.source_name}:${card.source_line}`;
    let profile;
    try {
      profile = loadProfile(repository) ?? { version: 1 as const, instructions: [], workflows: [] };
      if (bytes(card.text) > 1000) return setError("Card text exceeds the 1000-byte working instruction limit; edit it via Working style instead (it will not be truncated)");
      if (bytes(source) > 500) return setError("Card source exceeds the 500-byte limit; edit it via Working style instead (it will not be truncated)");
      if (profile.instructions.some((i) => i.text === card.text && i.source === source)) return setError("This instruction is already retained in the working profile");
      profile = { ...profile, instructions: [...profile.instructions, { text: card.text, source }] };
      saveProfile(repository, profile);
    } catch (reason) {
      return setError(reason instanceof Error ? reason.message : "Could not retain instruction");
    }
    setRetained(`Retained as working instruction (source card ${card.source_name}:${card.source_line} selected for import, so provenance is saved with the task if you confirm import)`);
    if (!selected.has(`${card.source_name}:${card.source_line}`)) toggleCard(card);
  }

  function appendEvidence(card: EvidenceCard, target: "next_action" | "decisions") {
    setError("");
    const addition = `[${card.source_name}:${card.source_line}] ${card.text}\n`;
    if (bytes(context[target] + addition) > 4000) { setError(`"${target}" is limited to 4000 bytes`); return; }
    setContext({ ...context, [target]: context[target] + addition });
  }

  async function doImport() {
    if (!log || pending) return;
    const gen = ++generation.current;
    setError("");
    const trimmed = prompt.trim();
    if (!trimmed) return setError("Prompt is required");
    if (bytes(trimmed) > 8000) return setError("Prompt is limited to 8000 bytes");
    if (Object.values(context).some((v) => bytes(v) > 4000)) return setError("Context fields are limited to 4000 bytes each");
    if (bytes(sourceName) > 1000) return setError("Source name is limited to 1000 bytes");
    const chosen = log.messages.filter((m) => selected.has(key(m)));
    if (!chosen.length) return setError("Select at least one message");
    const values = [trimmed, sourceName, ...Object.values(context)];
    if (values.some((v) => v.includes("\u0000"))) return setError("Null characters are not allowed");
    const payload = { session_id: log.sessionId, source_name: sourceName, parent_thread_id: log.parentThreadId, prompt: trimmed, context, messages: chosen };
    if (bytes(JSON.stringify(payload)) > 1048576) return setError("Import payload exceeds 1 MiB");
    setPending(true);
    try {
      const imported = await api.importCodex(payload);
      if (gen !== generation.current) return;
      onImported?.(imported);
    } catch (reason) {
      if (gen !== generation.current) return;
      setError(reason instanceof Error ? reason.message : "Import failed");
    } finally { if (gen === generation.current) setPending(false); }
  }

  return <div className="import-conversation">
    <input type="file" accept=".jsonl,application/jsonl,text/plain" disabled={pending} aria-label="Codex JSONL log file" onChange={(event) => { const f = event.target.files?.[0]; if (f) void load(f); event.target.value = ""; }} />
    {log && <div className="import-preview">
      <p className="muted">Session {log.sessionId}{log.parentThreadId ? ` · parent ${log.parentThreadId}` : " · no parent thread"} · original cwd {log.cwd || "(unknown)"} · current repo {repository || "(none)"}</p>
      {warnings.length > 0 && <ul className="import-warnings">{warnings.map((w, i) => <li key={i}>{w}</li>)}</ul>}
      <label className="field">Original request<textarea value={prompt} disabled={pending} onChange={(e) => setPrompt(e.target.value)} /></label>
      {contextFieldList.map((f) => <label key={f} className="field">{f}<textarea value={context[f]} disabled={pending} onChange={(e) => setContext({ ...context, [f]: e.target.value })} /></label>)}
      <p className="muted">Proposed context edit before import. The raw file stays local; only selected visible evidence is saved. Checks are historical: re-importing an existing conversation returns the previous task without applying new edits.</p>
      <div className="evidence-list">{cards.map((c) => { const k = `${c.source_name}:${c.source_line}`; return <div key={k} className="evidence-card"><label><input type="checkbox" checked={selected.has(k)} disabled={pending} onChange={() => toggleCard(c)} /><span className="evidence-src">{c.source_name}:{c.source_line}</span><span className="evidence-role">{c.role === "user" ? "user instruction candidate" : c.role === "assistant" ? "assistant claim" : "Historical tool record (unverified here)"}</span><span className="evidence-text">{c.text}</span></label>{c.role === "user" && <button type="button" disabled={pending || !repository} onClick={() => retainInstruction(c)}>Retain as working instruction</button>}<button type="button" disabled={pending} onClick={() => appendEvidence(c, "next_action")}>Use as next action</button><button type="button" disabled={pending} onClick={() => appendEvidence(c, "decisions")}>Use as decision</button></div>; })}</div>
      {retained && <p className="muted" role="status">{retained}</p>}
      <button onClick={() => void doImport()} disabled={pending}>Import task</button>
    </div>}
    {error && <p className="error" role="alert">{error}</p>}
  </div>;
}
