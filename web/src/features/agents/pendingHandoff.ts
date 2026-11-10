export type PendingHandoffTarget = "codex" | "claude" | "opencode";

export type PendingHandoff = {
  version: 1;
  taskId: string;
  repository: string;
  sourceRunId: string;
  target: PendingHandoffTarget;
  model: string;
  prompt: string;
  fingerprint: string | null;
  requestId: string;
};

const KEY_PREFIX = "aegis:pending-handoff:";
const TARGETS: PendingHandoffTarget[] = ["codex", "claude", "opencode"];
const MODEL_PATTERN = /^[A-Za-z0-9][A-Za-z0-9_./:-]*$/;
const MAX_PROMPT_BYTES = 65536;
const MAX_REQUEST_ID_LENGTH = 100;
const MAX_SOURCE_RUN_ID_LENGTH = 200;
const MAX_MODEL_LENGTH = 200;
const MAX_FINGERPRINT_LENGTH = 200;

const storageKey = (repository: string, taskId: string): string =>
  KEY_PREFIX + JSON.stringify([repository, taskId]);

const containsNUL = (value: string): boolean => value.includes("\u0000");

function requireNonBlankString(record: Record<string, unknown>, field: string, maxLength?: number): string {
  const value = record[field];
  if (typeof value !== "string" || value.trim().length === 0 || containsNUL(value)) {
    throw new Error(`Pending handoff field '${field}' must be a non-blank string without NUL characters.`);
  }
  if (maxLength !== undefined && value.length > maxLength) {
    throw new Error(`Pending handoff field '${field}' exceeds ${maxLength} characters.`);
  }
  return value;
}

const encodePrompt = (prompt: string): Uint8Array => new TextEncoder().encode(prompt);

function validate(raw: string, repository: string, taskId: string): PendingHandoff {
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch (error) {
    throw new Error(`Pending handoff for repository '${repository}' task '${taskId}' is not valid JSON: ${(error as Error).message}. Raw value preserved in storage.`);
  }
  if (typeof parsed !== "object" || parsed === null) {
    throw new Error(`Pending handoff for repository '${repository}' task '${taskId}' is malformed: expected an object.`);
  }
  const record = parsed as Record<string, unknown>;
  if (record.version !== 1) {
    throw new Error(`Pending handoff for repository '${repository}' task '${taskId}' has unsupported version: ${String(record.version)}.`);
  }
  if (record.taskId !== taskId || record.repository !== repository) {
    throw new Error(`Pending handoff identity mismatch: expected repository '${repository}' task '${taskId}', found repository '${String(record.repository)}' task '${String(record.taskId)}'.`);
  }
  const sourceRunId = requireNonBlankString(record, "sourceRunId", MAX_SOURCE_RUN_ID_LENGTH);
  const target = requireNonBlankString(record, "target");
  const requestId = requireNonBlankString(record, "requestId", MAX_REQUEST_ID_LENGTH);
  const modelValue = record.model;
  if (typeof modelValue !== "string") {
    throw new Error("Pending handoff field 'model' must be a string.");
  }
  if (modelValue.length > MAX_MODEL_LENGTH || containsNUL(modelValue)) {
    throw new Error(`Pending handoff field 'model' must be at most ${MAX_MODEL_LENGTH} characters without NUL characters.`);
  }
  if (modelValue.length > 0 && !MODEL_PATTERN.test(modelValue)) {
    throw new Error("Pending handoff field 'model' does not match the required model name pattern.");
  }
  const prompt = record.prompt;
  if (typeof prompt !== "string" || prompt.trim().length === 0 || containsNUL(prompt)) {
    throw new Error("Pending handoff field 'prompt' must be a non-blank string without NUL characters.");
  }
  const fingerprint = record.fingerprint;
  if (typeof fingerprint === "string") {
    if (fingerprint.length === 0 || fingerprint.length > MAX_FINGERPRINT_LENGTH || containsNUL(fingerprint)) {
      throw new Error(`Pending handoff field 'fingerprint' must be a non-empty string of at most ${MAX_FINGERPRINT_LENGTH} characters without NUL characters.`);
    }
  } else if (fingerprint !== null) {
    throw new Error("Pending handoff field 'fingerprint' must be a string or null.");
  }
  if (encodePrompt(prompt).byteLength > MAX_PROMPT_BYTES) {
    throw new Error(`Pending handoff prompt exceeds ${MAX_PROMPT_BYTES} UTF-8 bytes.`);
  }
  if (!TARGETS.includes(target as PendingHandoffTarget)) {
    throw new Error(`Pending handoff target is not a known agent (expected one of: ${TARGETS.join(", ")}).`);
  }
  return {
    version: 1,
    taskId,
    repository,
    sourceRunId,
    target: target as PendingHandoffTarget,
    model: modelValue,
    prompt,
    fingerprint: fingerprint as string | null,
    requestId,
  };
}

export function loadPendingHandoff(repository: string, taskId: string): PendingHandoff | null {
  const key = storageKey(repository, taskId);
  let raw: string | null;
  try {
    raw = window.localStorage.getItem(key);
  } catch (error) {
    throw new Error(`Failed to read pending handoff storage key '${key}': ${(error as Error).message}`);
  }
  if (raw === null) {
    return null;
  }
  return validate(raw, repository, taskId);
}

const canonical = (handoff: PendingHandoff): string =>
  JSON.stringify([
    handoff.version,
    handoff.taskId,
    handoff.repository,
    handoff.sourceRunId,
    handoff.target,
    handoff.model,
    handoff.prompt,
    handoff.fingerprint,
    handoff.requestId,
  ]);

export function savePendingHandoff(handoff: PendingHandoff): void {
  if (handoff.version !== 1) {
    throw new Error(`Pending handoff must have version 1, got ${String(handoff.version)}.`);
  }
  validate(JSON.stringify(handoff), handoff.repository, handoff.taskId);
  const key = storageKey(handoff.repository, handoff.taskId);
  const existing = loadPendingHandoff(handoff.repository, handoff.taskId);
  if (existing !== null) {
    if (canonical(existing) !== canonical(handoff)) {
      throw new Error(`Pending handoff at '${key}' already holds a different pending record (request id '${existing.requestId}'); refusing to overwrite.`);
    }
    return;
  }
  try {
    window.localStorage.setItem(key, JSON.stringify(handoff));
  } catch (error) {
    throw new Error(`Failed to persist pending handoff to '${key}': ${(error as Error).message}`);
  }
}

export function removePendingHandoff(repository: string, taskId: string, expectedRequestId: string): void {
  const key = storageKey(repository, taskId);
  let existing: string | null;
  try {
    existing = window.localStorage.getItem(key);
  } catch (error) {
    throw new Error(`Failed to read pending handoff storage key '${key}': ${(error as Error).message}`);
  }
  if (existing === null) return;
  let current: PendingHandoff;
  try {
    current = validate(existing, repository, taskId);
  } catch (error) {
    throw new Error(`Pending handoff at '${key}' is malformed and cannot be removed: ${(error as Error).message}`);
  }
  if (current.requestId !== expectedRequestId) {
    throw new Error(`Pending handoff request id mismatch: expected '${expectedRequestId}', found '${current.requestId}'; not removing.`);
  }
  try {
    window.localStorage.removeItem(key);
  } catch (error) {
    throw new Error(`Failed to remove pending handoff at '${key}': ${(error as Error).message}`);
  }
}
