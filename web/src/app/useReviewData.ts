import { useEffect, useRef, useState } from "react";
import { api, type HandoffContext, type ProvenanceRecord, type TaskGraph, type VerificationRun } from "./api";

export function useReviewData(taskId?: string) {
  const [verification, setVerification] = useState<VerificationRun>();
  const [handoff, setHandoff] = useState<HandoffContext>();
  const [graph, setGraph] = useState<TaskGraph>();
  const [provenance, setProvenance] = useState<ProvenanceRecord[]>([]);
  const [error, setError] = useState("");
  const refreshSequence = useRef(0);
  const verificationSequence = useRef(0);
  const verifyRequestSequence = useRef(0);
  const currentTaskId = useRef(taskId);
  currentTaskId.current = taskId;

  async function refresh() {
    if (!taskId) return;
    const requestedTaskId = taskId;
    const sequence = ++refreshSequence.current;
    const currentVerificationSequence = verificationSequence.current;
    try {
      const [loadedHandoff, loadedGraph, loadedProvenance, verifications] = await Promise.all([
        api.handoff(requestedTaskId), api.graph(requestedTaskId), api.provenance(requestedTaskId), api.verifications(requestedTaskId),
      ]);
      if (sequence !== refreshSequence.current || requestedTaskId !== currentTaskId.current) return;
      setHandoff(loadedHandoff);
      setGraph(loadedGraph);
      setProvenance(loadedProvenance.records);
      if (currentVerificationSequence === verificationSequence.current) setVerification(verifications[0]);
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
  }, [taskId]);

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
