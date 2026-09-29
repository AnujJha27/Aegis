import { useEffect, useRef, useState } from "react";
import { api, type HandoffContext, type ProvenanceRecord, type TaskGraph, type VerificationRun } from "./api";

export function useReviewData(taskId?: string, view?: "review" | "graphs" | "activity") {
  const [verification, setVerification] = useState<VerificationRun>();
  const [handoff, setHandoff] = useState<HandoffContext>();
  const [graph, setGraph] = useState<TaskGraph>();
  const [provenance, setProvenance] = useState<ProvenanceRecord[]>([]);
  const [error, setError] = useState("");
  const refreshSequence = useRef(0);
  const verificationSequence = useRef(0);
  const verifyRequestSequence = useRef(0);
  const currentTaskId = useRef(taskId);
  const currentView = useRef(view);
  currentTaskId.current = taskId;
  currentView.current = view;

  async function refresh() {
    if (!taskId) return;
    const requestedTaskId = taskId;
    const sequence = ++refreshSequence.current;
    const currentVerificationSequence = verificationSequence.current;
    const requestedView = currentView.current;
    try {
      const [loadedHandoff, loadedGraph, loadedProvenance, verifications] = await Promise.all([
        requestedView === "review" || requestedView === "activity" ? api.handoff(requestedTaskId) : undefined,
        requestedView === "graphs" ? api.graph(requestedTaskId) : undefined,
        requestedView === "activity" ? api.provenance(requestedTaskId) : undefined,
        requestedView === "review" || requestedView === "activity" ? api.verifications(requestedTaskId) : undefined,
      ]);
      if (sequence !== refreshSequence.current || requestedTaskId !== currentTaskId.current) return;
      if (loadedHandoff) setHandoff(loadedHandoff);
      if (loadedGraph) setGraph(loadedGraph);
      if (loadedProvenance) setProvenance(loadedProvenance.records);
      if (verifications && currentVerificationSequence === verificationSequence.current) setVerification(verifications[0]);
      setError("");
    } catch (reason) {
      if (sequence === refreshSequence.current && requestedTaskId === currentTaskId.current)
        setError(reason instanceof Error ? reason.message : "Could not load task review data");
      throw reason;
    }
  }

  useEffect(() => {
    ++refreshSequence.current;
    ++verificationSequence.current;
    ++verifyRequestSequence.current;
    setVerification(undefined);
    setHandoff(undefined);
    setGraph(undefined);
    setProvenance([]);
    setError("");
    if (taskId) void refresh().catch(() => {});
  }, [taskId, view]);

  async function verify(runId?: string) {
    if (!taskId) return undefined;
    const requestedTaskId = taskId;
    const requestSequence = ++verifyRequestSequence.current;
    const result = await api.verify(requestedTaskId, ["ctest", "--test-dir", "build"], runId);
    if (requestedTaskId === currentTaskId.current && requestSequence === verifyRequestSequence.current) {
      ++verificationSequence.current;
      setVerification(result);
    }
    return result;
  }

  return { verification, setVerification, handoff, graph, provenance, error, refresh, verify };
}
