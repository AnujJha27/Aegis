import { useEffect, useMemo, useRef, useState } from "react";
import { api, type AgentEvent, type AgentRun } from "./api";
import { connectEvents } from "./events";

export function useAgentWorkspace(taskId?: string, onEvent?: (event: AgentEvent) => void) {
  const [runs, setRuns] = useState<AgentRun[]>([]);
  const [selectedRunId, setSelectedRunId] = useState("");
  const [events, setEvents] = useState<AgentEvent[]>([]);
  const onEventRef = useRef(onEvent);
  onEventRef.current = onEvent;

  useEffect(() => {
    setRuns([]);
    setEvents([]);
    setSelectedRunId("");
    if (!taskId) return;
    let active = true;
    let snapshotLoaded = false;
    const pending: AgentEvent[] = [];
    const disconnect = connectEvents((event) => {
      onEventRef.current?.(event);
      if (event.taskId !== taskId) return;
      const lifecycle = event.type.startsWith("run.") ? event.type.slice(4) : "";
      if (["started", "completed", "failed", "interrupted", "terminated"].includes(lifecycle)) {
        setRuns((current) => current.map((run) => run.id === event.runId ? { ...run, status: lifecycle, finishedAt: lifecycle === "started" ? 0 : event.timestamp } : run));
      }
      if (snapshotLoaded) setEvents((current) => current.some((item) => item.id === event.id) ? current : [...current.slice(-499), event]);
      else pending.push(event);
    });
    Promise.all([api.events(taskId), api.runs(taskId)]).then(([loadedEvents, loadedRuns]) => {
      if (!active) return;
      snapshotLoaded = true;
      setEvents([...loadedEvents, ...pending.filter((item) => !loadedEvents.some((event) => event.id === item.id))].slice(-500));
      setRuns(loadedRuns);
      setSelectedRunId(loadedRuns.at(-1)?.id ?? "");
    });
    return () => { active = false; disconnect(); };
  }, [taskId]);

  const currentRun = runs.find((run) => run.id === selectedRunId) ?? runs.at(-1);
  const currentEvents = useMemo(() => taskId ? events.filter((event) => event.taskId === taskId) : [], [events, taskId]);
  const currentRunEvents = useMemo(() => currentRun ? currentEvents.filter((event) => event.runId === currentRun.id) : [], [currentEvents, currentRun]);
  const runFinished = currentRun ? ["completed", "failed", "interrupted", "terminated"].includes(currentRun.status) : false;

  return { runs, setRuns, selectedRunId, setSelectedRunId, events, setEvents, currentRun, currentEvents, currentRunEvents, runFinished };
}
