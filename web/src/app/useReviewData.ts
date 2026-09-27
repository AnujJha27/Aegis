import { useEffect, useState } from "react";
import { api, type HandoffContext, type ProvenanceRecord, type TaskGraph, type VerificationRun } from "./api";

export function useReviewData(taskId?: string) {
  const [verification, setVerification] = useState<VerificationRun>();
  const [handoff, setHandoff] = useState<HandoffContext>();
  const [graph, setGraph] = useState<TaskGraph>();
  const [provenance, setProvenance] = useState<ProvenanceRecord[]>([]);

  async function refresh() {
    if (!taskId) return;
    const [loadedHandoff, loadedGraph, loadedProvenance, verifications] = await Promise.all([
      api.handoff(taskId), api.graph(taskId), api.provenance(taskId), api.verifications(taskId),
    ]);
    setHandoff(loadedHandoff);
    setGraph(loadedGraph);
    setProvenance(loadedProvenance.records);
    setVerification(verifications[0]);
  }

  useEffect(() => { void refresh(); }, [taskId]);

  async function verify(runId?: string) {
    if (!taskId) return undefined;
    const result = await api.verify(taskId, ["ctest", "--test-dir", "build"], runId);
    setVerification(result);
    return result;
  }

  return { verification, setVerification, handoff, graph, provenance, refresh, verify };
}
