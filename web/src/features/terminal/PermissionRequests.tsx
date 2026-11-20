import { useCallback, useEffect, useRef, useState } from "react";
import type { AgentEvent } from "../../app/api";
import { api } from "../../app/api";

type ParsedRequest = { requestId: string; toolName: string; input: string };

function parseRequest(event: AgentEvent): ParsedRequest | null {
  try {
    const parsed = JSON.parse(event.content) as { request_id?: unknown; tool_name?: unknown; input?: unknown };
    if (typeof parsed !== "object" || parsed === null || typeof parsed.request_id !== "string") return null;
    const input = typeof parsed.input === "string" ? parsed.input : JSON.stringify(parsed.input ?? null, null, 2);
    return { requestId: parsed.request_id, toolName: typeof parsed.tool_name === "string" ? parsed.tool_name : "unknown", input };
  } catch {
    return null;
  }
}

export function PermissionRequests({ runId, runFinished, events }: { runId: string; runFinished: boolean; events: AgentEvent[] }) {
  const [pending, setPending] = useState<AgentEvent[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [submitting, setSubmitting] = useState<string | null>(null);
  const deciding = useRef(false);
  const unmounted = useRef(false);
  const keyedRun = useRef(runId);
  const seq = useRef(0);
  const lastEventId = events.length ? events[events.length - 1].id : "";

  useEffect(() => {
    unmounted.current = false;
    return () => { unmounted.current = true; seq.current++; };
  }, []);

  const refresh = useCallback(async (): Promise<boolean> => {
    const ticket = ++seq.current;
    try {
      const result = await api.pendingPermissions(runId);
      if (unmounted.current || keyedRun.current !== runId || ticket !== seq.current) return false;
      setPending(result); setError(null);
      return true;
    } catch (err: unknown) {
      if (unmounted.current || keyedRun.current !== runId || ticket !== seq.current) return false;
      setPending([]); setError(err instanceof Error ? err.message : "Failed to load pending permission requests — recheck after reconnect");
      return false;
    }
  }, [runId]);

  useEffect(() => {
    if (keyedRun.current !== runId) { keyedRun.current = runId; setPending([]); setError(null); setSubmitting(null); }
    if (!runFinished && !deciding.current) void refresh();
  }, [runId, runFinished, lastEventId, refresh]);

  useEffect(() => {
    if (runFinished) return;
    const timer = window.setInterval(() => { if (!unmounted.current && !deciding.current) void refresh(); }, 2000);
    return () => window.clearInterval(timer);
  }, [runFinished, refresh]);

  const respond = async (event: AgentEvent, requestId: string, allow: boolean) => {
    if (deciding.current || submitting || event.runId !== runId) return;
    deciding.current = true;
    setSubmitting(requestId);
    seq.current++;
    let postError: string | null = null;
    try {
      await api.respondPermission(runId, requestId, allow);
    } catch (err) {
      postError = err instanceof Error && err.message ? err.message : "Unknown result — recheck pending requests before retrying";
    }
    const refreshed = await refresh();
    if (!unmounted.current && keyedRun.current === runId) {
      if (postError) setError(refreshed ? `Decision could not be confirmed: ${postError} — pending list was rechecked` : postError);
      setSubmitting(null);
    }
    deciding.current = false;
  };

  if (runFinished || (!pending.length && !error)) return null;
  const requests = pending.map((event) => ({ event, parsed: parseRequest(event) })).filter((entry): entry is { event: AgentEvent; parsed: ParsedRequest } => entry.parsed !== null);
  return <section className="permission-requests">
    <h2>Pending permission requests</h2>
    {error && <p className="permission-error" role="alert">{error}</p>}
    {requests.map(({ event, parsed }) => <article className="permission-request" key={event.id}>
      <div className="permission-meta"><span>{parsed.toolName}</span><time>{new Date(event.timestamp).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}</time></div>
      <pre>{parsed.input}</pre>
      <div className="permission-actions">
        <button onClick={() => respond(event, parsed.requestId, true)} disabled={submitting !== null}>Allow once</button>
        <button onClick={() => respond(event, parsed.requestId, false)} disabled={submitting !== null}>Deny</button>
      </div>
    </article>)}
  </section>;
}
