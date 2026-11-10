import { useCallback, useEffect, useRef, useState } from "react";
import { api, type AgentEvent } from "../../app/api";
import { readable } from "../../app/events";

type Props = { taskId: string; beforeSequence: number | undefined };

export default function OlderEventHistory({ taskId, beforeSequence }: Props) {
  const [rows, setRows] = useState<AgentEvent[]>([]);
  const [cursor, setCursor] = useState<number | undefined>(beforeSequence);
  const [hasMore, setHasMore] = useState(beforeSequence !== undefined);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const mounted = useRef(true);

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
    };
  }, []);

  const load = useCallback(async () => {
    if (cursor === undefined || loading) return;
    setLoading(true);
    setError(null);
    try {
      const page = await api.eventsBefore(taskId, cursor, 100);
      if (!mounted.current) return;
      setRows((prev) => [...prev, ...page.events]);
      setCursor(page.next_cursor ?? undefined);
      setHasMore(page.has_more && page.events.length > 0 && page.next_cursor !== null);
    } catch (err) {
      if (mounted.current) setError(err instanceof Error ? err.message : String(err));
    } finally {
      if (mounted.current) setLoading(false);
    }
  }, [taskId, cursor, loading]);

  if (beforeSequence === undefined) {
    return <p>older source history unavailable: sequence missing for task {taskId}</p>;
  }

  return (
    <details>
      <summary>older history ({rows.length})</summary>
      {rows.map((event) => (
        <div key={event.id}>
          <span>{event.id}</span> <span>{event.type}</span> <span>{event.agent}</span> <span>{event.runId}</span>{" "}
          <span>{new Date(event.timestamp).toLocaleString()}</span>
          <p>{readable(event.content).slice(0, 200)}</p>
        </div>
      ))}
      {error && <p>failed to load older history for task {taskId}: {error}</p>}
      {hasMore && <button onClick={() => void load()} disabled={loading}>{error ? "Retry" : rows.length > 0 ? "Load more older events" : "Load older events"}</button>}
    </details>
  );
}