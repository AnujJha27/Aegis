import { useCallback, useEffect, useRef, useState } from "react";
import { api, type AgentRun, type Task } from "../../app/api";
import { loadPendingHandoff, removePendingHandoff, type PendingHandoff } from "./pendingHandoff";

type Props = {
  task: Task;
  sourceBusy: boolean;
  onContinued?: (run: AgentRun, sourceRunId: string) => void;
  onPendingChange?: (exists: boolean) => void;
};

type RecoveryState =
  | { kind: "idle" }
  | { kind: "pending"; record: PendingHandoff }
  | { kind: "notconfirmed"; record: PendingHandoff; message: string }
  | { kind: "uncertain"; record: PendingHandoff; message: string }
  | { kind: "malformed"; message: string };

const describeRecord = (record: PendingHandoff) => [
  ["Prompt", record.prompt],
  ["Source", record.sourceRunId],
  ["Target", record.target],
  ["Model", record.model ?? "provider default"],
  ["Request ID", record.requestId],
] as const;

export default function HandoffRecovery({ task, sourceBusy, onContinued, onPendingChange }: Props) {
  const [state, setState] = useState<RecoveryState>({ kind: "idle" });
  const [busy, setBusy] = useState(false);
  const pendingRef = useRef(false);
  const generationRef = useRef(0);
  const onContinuedRef = useRef(onContinued);
  const onPendingChangeRef = useRef(onPendingChange);
  onContinuedRef.current = onContinued;
  onPendingChangeRef.current = onPendingChange;

  const reportPending = useCallback((exists: boolean) => {
    onPendingChangeRef.current?.(exists);
  }, []);

  useEffect(() => {
    const generation = ++generationRef.current;
    pendingRef.current = false;
    setBusy(false);
    return () => {
      if (generationRef.current === generation) generationRef.current += 1;
    };
  }, [task.id, task.repository]);

  useEffect(() => {
    let cancelled = false;
    let exists = false;
    try {
      const record = loadPendingHandoff(task.repository, task.id);
      if (record) {
        exists = true;
        if (!cancelled) setState({ kind: "pending", record });
      } else if (!cancelled) {
        setState({ kind: "idle" });
      }
    } catch (error) {
      exists = true;
      if (!cancelled) {
        setState({ kind: "malformed", message: error instanceof Error ? `${error.message} A stored handoff continuation request could not be read. It is preserved but new continuations are blocked until it is resolved.` : "Stored handoff request is unreadable. It is preserved but new continuations are blocked until it is resolved." });
      }
    }
    if (!cancelled) reportPending(exists);
    return () => {
      cancelled = true;
    };
  }, [task.id, task.repository, reportPending]);

  const finishClear = useCallback((record: PendingHandoff, generation: number) => {
    if (generationRef.current !== generation) return false;
    try {
      removePendingHandoff(record.repository, record.taskId, record.requestId);
    } catch {
      setState({ kind: "uncertain", record, message: "The handoff request could not be cleared from local storage. It will be offered again." });
      return false;
    }
    reportPending(false);
    setState({ kind: "idle" });
    return true;
  }, [reportPending]);

  const checkDelivery = useCallback(async (record: PendingHandoff) => {
    if (pendingRef.current) return;
    const generation = generationRef.current;
    pendingRef.current = true;
    setBusy(true);
    try {
      const receipt = await api.continuationReceipt(record.taskId, record.requestId);
      if (generationRef.current !== generation) return;
      if (receipt && receipt.run_id && receipt.state === "delivered") {
        const runs = await api.runs(record.taskId);
        if (generationRef.current !== generation) return;
        const found = runs.find((r) => r.id === receipt.run_id);
        if (found) {
          if (finishClear(record, generation)) onContinuedRef.current?.(found, record.sourceRunId);
          return;
        }
        setState({ kind: "uncertain", record, message: `The delivery receipt references run ${receipt.run_id}, which was not found. The record is preserved.` });
        return;
      }
      setState({ kind: "uncertain", record, message: `Delivery could not be confirmed (state: ${receipt?.state ?? "unknown"}). The record is preserved.` });
    } catch (error) {
      if (generationRef.current !== generation) return;
      const status = (error as { status?: number }).status;
      if (status === 404) {
        setState({ kind: "notconfirmed", record, message: "No delivery receipt exists for this request. You can retry the exact request below." });
      } else {
        setState({ kind: "uncertain", record, message: `Delivery check failed: ${error instanceof Error ? error.message : "unknown error"}. The record is preserved.` });
      }
    } finally {
      if (generationRef.current === generation) {
        pendingRef.current = false;
        setBusy(false);
      }
    }
  }, [finishClear]);

  const retry = useCallback(async (record: PendingHandoff) => {
    if (pendingRef.current) return;
    const generation = generationRef.current;
    pendingRef.current = true;
    setBusy(true);
    try {
      const result = await api.continueTask(record.taskId, record.target, record.sourceRunId, record.prompt, record.requestId, record.fingerprint, record.model || undefined);
      if (generationRef.current !== generation) return;
      if (finishClear(record, generation)) onContinuedRef.current?.(result, record.sourceRunId);
    } catch (error) {
      if (generationRef.current !== generation) return;
      setState({ kind: "uncertain", record, message: `Retry failed: ${error instanceof Error ? error.message : "unknown error"}. The request is preserved.` });
    } finally {
      if (generationRef.current === generation) {
        pendingRef.current = false;
        setBusy(false);
      }
    }
  }, [finishClear]);

  const forget = useCallback((record: PendingHandoff) => {
    if (!window.confirm("Forget this handoff request? The agent may have already been launched. Forgetting it means a future manual launch could duplicate the work.")) return;
    try {
      removePendingHandoff(record.repository, record.taskId, record.requestId);
      reportPending(false);
      setState({ kind: "idle" });
    } catch {
      setState({ kind: "uncertain", record, message: "The handoff request could not be removed from local storage." });
    }
  }, [reportPending]);

  if (state.kind === "idle") return null;

  const blocked = busy || pendingRef.current;

  return (
    <section className="handoff-recovery" role="region" aria-label="Pending handoff recovery">
      {state.kind === "malformed" ? (
        <>
          <p className="handoff-recovery-error">{state.message}</p>
          <p>Stored handoff request data is malformed. No automatic send will occur. Clear the stored entry manually to continue.</p>
        </>
      ) : (
        <>
          {state.kind !== "pending" && <p className="handoff-recovery-status">{state.message}</p>}
          {state.kind === "uncertain" && <p className="handoff-recovery-warning">Delivery status is uncertain. The request has been preserved; do not launch a duplicate manually until resolved.</p>}
          <dl className="handoff-recovery-details">
            {describeRecord(state.record).map(([label, value]) => (
              <div key={label}>
                <dt>{label}</dt>
                <dd>{value}</dd>
              </div>
            ))}
          </dl>
          <div className="handoff-recovery-actions">
            {(state.kind === "pending" || state.kind === "uncertain") && (
              <button type="button" disabled={blocked} onClick={() => checkDelivery(state.record)}>
                Check delivery
              </button>
            )}
            <button type="button" disabled={blocked || sourceBusy} onClick={() => retry(state.record)}>
              Retry same request
            </button>
            <button type="button" disabled={blocked} onClick={() => forget(state.record)}>
              Forget request
            </button>
          </div>
        </>
      )}
    </section>
  );
}
