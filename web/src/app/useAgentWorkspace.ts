import { useEffect, useMemo, useRef, useState } from "react";
import { api, type AgentEvent, type AgentRun } from "./api";
import { connectEvents, type EventConnection } from "./events";

export function useAgentWorkspace(taskId?: string, onEvent?: (event: AgentEvent) => void, onReconnect?: () => void) {
  const [runs, setRuns] = useState<AgentRun[]>([]);
  const [selectedRunId, setSelectedRunId] = useState("");
  const [events, setEvents] = useState<AgentEvent[]>([]);
  const [connection, setConnection] = useState<EventConnection>("connecting");
  const onEventRef = useRef(onEvent);
  const onReconnectRef = useRef(onReconnect);
  onEventRef.current = onEvent;
  onReconnectRef.current = onReconnect;

  useEffect(() => {
    setRuns([]);
    setEvents([]);
    setSelectedRunId("");
    if (!taskId) return;
    let active = true;
    let snapshotLoaded = false;
    let snapshotsInFlight = 0;
    let snapshotSequence = 0;
    let appliedSnapshot = 0;
    const pending: AgentEvent[] = [];
    const refreshSnapshot = async () => {
      const sequence = ++snapshotSequence;
      snapshotsInFlight++;
      try {
        const [loadedEvents, loadedRuns] = await Promise.all([api.events(taskId), api.runs(taskId)]);
        if (!active || sequence < appliedSnapshot) return;
        appliedSnapshot = sequence;
        const unique = new Map([...loadedEvents, ...pending].map((event) => [event.id, event]));
        snapshotLoaded = true;
        setEvents([...unique.values()].slice(-500));
        setRuns(loadedRuns);
        setSelectedRunId((current) => loadedRuns.some((run) => run.id === current) ? current : loadedRuns.at(-1)?.id ?? "");
      } catch { /* The connection indicator reports the failure; the next reconnect retries the snapshot. */ }
      finally {
        if (--snapshotsInFlight === 0) pending.length = 0;
      }
    };
    const disconnect = connectEvents((event) => {
      onEventRef.current?.(event);
      if (event.taskId !== taskId) return;
      const lifecycle = event.type.startsWith("run.") ? event.type.slice(4) : "";
      if (["started", "completed", "failed", "interrupted", "terminated"].includes(lifecycle)) {
        const status = lifecycle === "started" ? "running" : lifecycle;
        setRuns((current) => current.map((run) => run.id === event.runId ? { ...run, status, finishedAt: lifecycle === "started" ? 0 : event.timestamp } : run));
      }
      if (!snapshotLoaded || snapshotsInFlight > 0) {
        pending.push(event);
        if (pending.length > 500) pending.shift();
      }
      if (snapshotLoaded && event.type !== "terminal.output") setEvents((current) => current.some((item) => item.id === event.id) ? current : [...current.slice(-499), event]);
    }, (state, reconnected) => {
      setConnection(state);
      if (reconnected) { void refreshSnapshot().then(() => onReconnectRef.current?.()); }
    });
    void refreshSnapshot();
    return () => { active = false; disconnect(); };
  }, [taskId]);

  const currentRun = runs.find((run) => run.id === selectedRunId) ?? runs.at(-1);
  const currentEvents = useMemo(() => taskId ? events.filter((event) => event.taskId === taskId) : [], [events, taskId]);
  const currentRunEvents = useMemo(() => currentRun ? currentEvents.filter((event) => event.runId === currentRun.id) : [], [currentEvents, currentRun]);
  const runFinished = currentRun ? ["completed", "failed", "interrupted", "terminated"].includes(currentRun.status) : false;

  return { runs, setRuns, selectedRunId, setSelectedRunId, events, setEvents, currentRun, currentEvents, currentRunEvents, runFinished, connection };
}
