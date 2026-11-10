import { useCallback, useEffect, useRef, useState } from "react";
import type { Task, TaskContext } from "../../app/api";

export type TaskWithRevision = Task & { context_revision?: number };

const emptyContext: TaskContext = { goal: "", constraints: "", decisions: "", attempts: "", unfinished: "", next_action: "" };
const fields: (keyof TaskContext)[] = ["goal", "constraints", "decisions", "attempts", "unfinished", "next_action"];

const storageKey = (repository: string, id: string) => `aegis:context-draft:${JSON.stringify([repository, id])}`;

const isSafeInteger = (value: unknown) => typeof value === "number" && Number.isSafeInteger(value);

const normalizeContext = (value: unknown): TaskContext | null => {
  if (typeof value !== "object" || value === null) return null;
  const source = value as Record<string, unknown>;
  const out = {} as TaskContext;
  for (const field of fields) {
    if (typeof source[field] !== "string") return null;
    out[field] = source[field] as string;
  }
  return out;
};

type Record_ = { version: number; baseRevision: number; context: TaskContext; editedAt: string };

const parseRecord = (raw: string): Record_ | null => {
  if (raw.length > 1048576) return null;
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return null;
  }
  if (typeof parsed !== "object" || parsed === null) return null;
  const source = parsed as Record<string, unknown>;
  if (source.version !== 1) return null;
  if (!isSafeInteger(source.baseRevision) || (source.baseRevision as number) < 0) return null;
  const context = normalizeContext(source.context);
  if (!context) return null;
  return { version: 1, baseRevision: source.baseRevision as number, context, editedAt: typeof source.editedAt === "string" ? source.editedAt : "" };
};

export function useContextDraft(task: TaskWithRevision) {
  const [draft, setDraftState] = useState<TaskContext>(emptyContext);
  const [revision, setRevisionState] = useState(0);
  const [dirty, setDirtyState] = useState(false);
  const [conflict, setConflict] = useState<Task | null>(null);
  const [storageError, setStorageError] = useState("");

  const draftRef = useRef<TaskContext>(emptyContext);
  const dirtyRef = useRef(false);
  const baseRevisionRef = useRef(0);
  const lastOwnedRawRef = useRef<string | null>(null);
  const identityRef = useRef({ repository: task.repository, id: task.id });
  const malformedRef = useRef(false);

  const setDraft = (next: TaskContext) => { draftRef.current = next; setDraftState(next); };
  const setRevision = (next: number) => { setRevisionState(next); };
  const setDirty = (next: boolean) => { dirtyRef.current = next; setDirtyState(next); };

  const readStored = useCallback((): { raw: string; record: Record_ | null } => {
    let raw = "";
    try {
      raw = window.localStorage.getItem(storageKey(task.repository, task.id)) ?? "";
    } catch (err) {
      setStorageError(`Draft storage: ${err instanceof Error ? err.message : String(err)}`);
      return { raw: "", record: null };
    }
    return { raw, record: raw === "" ? null : parseRecord(raw) };
  }, [task.repository, task.id]);

  const persist = useCallback((context: TaskContext, baseRevision: number, force: boolean): boolean => {
    const key = storageKey(task.repository, task.id);
    let existing: string | null = null;
    try {
      existing = window.localStorage.getItem(key);
    } catch (err) {
      setStorageError(`Draft storage: ${err instanceof Error ? err.message : String(err)}`);
      return false;
    }
    if (malformedRef.current) {
      setStorageError("Draft storage: stored draft is malformed; discard it before saving");
      return false;
    }
    if (!force && existing !== null && existing !== lastOwnedRawRef.current) {
      setStorageError("Draft storage was modified by another tab; local draft preserved");
      return false;
    }
    const record: Record_ = { version: 1, baseRevision, context, editedAt: new Date().toISOString() };
    const raw = JSON.stringify(record);
    try {
      window.localStorage.setItem(key, raw);
    } catch (err) {
      setStorageError(`Draft storage: ${err instanceof Error ? err.message : String(err)}`);
      return false;
    }
    lastOwnedRawRef.current = raw;
    setStorageError("");
    return true;
  }, [task.repository, task.id]);

  // Load draft on mount and task identity change.
  useEffect(() => {
    if (identityRef.current.repository !== task.repository || identityRef.current.id !== task.id) {
      identityRef.current = { repository: task.repository, id: task.id };
      lastOwnedRawRef.current = null;
      setConflict(null);
    }
    setStorageError("");
    const { raw, record } = readStored();
    if (record) {
      lastOwnedRawRef.current = raw;
      baseRevisionRef.current = record.baseRevision;
      setDraft(record.context);
      setRevision(record.baseRevision);
      setDirty(true);
      malformedRef.current = false;
    } else {
      if (raw !== "") setStorageError("Draft storage: stored draft is malformed; kept until replaced or discarded");
      lastOwnedRawRef.current = raw === "" ? null : raw;
      malformedRef.current = raw !== "";
      setDraft({ ...emptyContext, ...(task.context ?? {}) });
      setRevision(task.context_revision ?? 0);
      baseRevisionRef.current = task.context_revision ?? 0;
      setDirty(false);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [task.repository, task.id]);

  // Server-side revision bump while local draft is clean: adopt saved context.
  useEffect(() => {
    const serverRevision = task.context_revision ?? 0;
    if (!dirtyRef.current && serverRevision > baseRevisionRef.current && conflict === null) {
      baseRevisionRef.current = serverRevision;
      setRevision(serverRevision);
      setDraft({ ...emptyContext, ...(task.context ?? {}) });
      setStorageError("");
    } else if (serverRevision > baseRevisionRef.current && dirtyRef.current && conflict === null) {
      setConflict({ ...task });
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [task.context_revision, task.repository, task.id]);

  const edit = useCallback((next: TaskContext) => {
    if (identityRef.current.repository !== task.repository || identityRef.current.id !== task.id) return;
    setDraft(next);
    setDirty(true);
    persist(next, baseRevisionRef.current, false);
  }, [task.repository, task.id, persist]);

  // Cleanup owned storage only when the current raw record matches the last one we wrote.
  const discardStored = useCallback((): boolean => {
    const key = storageKey(task.repository, task.id);
    let current: string | null = null;
    try {
      current = window.localStorage.getItem(key);
      if (current === lastOwnedRawRef.current) {
        window.localStorage.removeItem(key);
        lastOwnedRawRef.current = null;
        malformedRef.current = false;
        setStorageError("");
        return true;
      }
      setStorageError("Stored draft was changed by another tab; not removed");
      return false;
    } catch (err) {
      setStorageError(`Draft storage: ${err instanceof Error ? err.message : String(err)}`);
      return false;
    }
  }, [task.repository, task.id]);

  const saved = useCallback((savedTask: Task, submitted: TaskContext): boolean => {
    if (identityRef.current.repository !== savedTask.repository || identityRef.current.id !== savedTask.id) return false;
    const savedWithRevision = savedTask as TaskWithRevision;
    const nextRevision = savedWithRevision.context_revision ?? baseRevisionRef.current;
    if (isSafeInteger(nextRevision) && nextRevision < baseRevisionRef.current) {
      return false; // stale response; never adopt lower revision
    }
    const sameDraft = (a: TaskContext, b: TaskContext) => fields.every((f) => a[f] === b[f]);
    if (!dirtyRef.current || sameDraft(draftRef.current, submitted)) {
      discardStored();
      baseRevisionRef.current = nextRevision;
      setRevision(nextRevision);
      setDraft({ ...emptyContext, ...((savedTask.context ?? submitted) as TaskContext) });
      setDirty(false);
      setConflict(null);
      return true;
    }
    // User edited while request was in flight: rebase local draft onto new baseline.
    baseRevisionRef.current = nextRevision;
    setRevision(nextRevision);
    persist(draftRef.current, nextRevision, false);
    return false;
  }, [persist, discardStored]);

  const resolveConflict = useCallback((useSaved: boolean) => {
    const conflictTask = conflict;
    if (!conflictTask) return;
    if (identityRef.current.repository !== conflictTask.repository || identityRef.current.id !== conflictTask.id) return;
    const nextRevision = (conflictTask as TaskWithRevision).context_revision ?? baseRevisionRef.current;
    if (useSaved) {
      baseRevisionRef.current = nextRevision;
      setRevision(nextRevision);
      setDraft({ ...emptyContext, ...(conflictTask.context ?? {}) });
      setDirty(false);
      discardStored();
    } else {
      baseRevisionRef.current = nextRevision;
      setRevision(nextRevision);
      persist(draftRef.current, nextRevision, false);
    }
    setConflict(null);
  }, [conflict, persist, discardStored]);

  const acceptConflict = useCallback((authoritative: Task) => {
    if (identityRef.current.repository !== authoritative.repository || identityRef.current.id !== authoritative.id) return;
    setConflict(authoritative);
  }, []);

  return {
    draft,
    edit,
    revision,
    dirty,
    conflict,
    storageError,
    resolveConflict,
    acceptConflict,
    saved,
    discardStored,
  };
}
