export type ProfileInstruction = { text: string; source: string };
export type WorkflowStep = { purpose: string; context: string; agent: string; result: string; evidenceNeeded: string };
export type Workflow = { name: string; steps: WorkflowStep[] };
export type WorkingProfile = { version: 1; instructions: ProfileInstruction[]; workflows: Workflow[] };

const KEY_PREFIX = "aegis:profile:";
const ENCODER = new TextEncoder();
const DECODER = new TextDecoder();
const byteLen = (value: string) => ENCODER.encode(value).length;

export function profileStorageKey(repository: string): string {
  return KEY_PREFIX + repository.trim();
}

function defaultStorage(): Storage | null {
  return typeof localStorage === "undefined" ? null : localStorage;
}

export function validateProfile(candidate: unknown): WorkingProfile {
  if (!candidate || typeof candidate !== "object" || Array.isArray(candidate)) throw new Error("Profile must be an object");
  const profile = candidate as WorkingProfile;
  if (profile.version !== 1) throw new Error("Unsupported profile version (expected 1)");
  if (!Array.isArray(profile.instructions) || profile.instructions.length > 20) throw new Error("Profile needs at most 20 instructions");
  for (const instruction of profile.instructions) {
    if (!instruction || typeof instruction.text !== "string" || typeof instruction.source !== "string") throw new Error("Each instruction needs text and source strings");
    if (!instruction.text.trim()) throw new Error("Instruction text must not be blank");
    if (instruction.text.includes("\u0000") || instruction.source.includes("\u0000")) throw new Error("Instructions must not contain NUL characters");
    if (byteLen(instruction.text) > 1000) throw new Error("Instruction text exceeds 1000 UTF-8 bytes");
    if (byteLen(instruction.source) > 500) throw new Error("Instruction source exceeds 500 UTF-8 bytes");
  }
  if (!Array.isArray(profile.workflows)) throw new Error("Workflows must be an array");
  if (profile.workflows.length > 5) throw new Error("At most 5 workflows per profile");
  for (const workflow of profile.workflows) {
    if (!workflow || typeof workflow.name !== "string" || !Array.isArray(workflow.steps)) throw new Error("Each workflow needs a name and a steps array");
    if (workflow.steps.length > 10) throw new Error(`Workflow "${workflow.name}" has more than 10 steps`);
    for (const step of workflow.steps) {
      for (const key of ["purpose", "context", "agent", "result", "evidenceNeeded"] as const) {
        if (typeof step?.[key] !== "string") throw new Error(`Workflow "${workflow.name}" step field ${key} must be a string`);
        if (byteLen(step[key]) > 500) throw new Error(`Workflow "${workflow.name}" step field ${key} exceeds 500 UTF-8 bytes`);
      }
    }
  }
  if (byteLen(JSON.stringify(profile)) > 8000) throw new Error("Profile exceeds 8000 UTF-8 bytes");
  return profile;
}

export function loadProfile(repository: string, storage: Storage | null = defaultStorage()): WorkingProfile | null {
  if (!storage) return null;
  const raw = storage.getItem(profileStorageKey(repository));
  if (raw === null) return null;
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    throw new Error("Saved working profile is malformed (kept as-is; fix or clear it manually in this browser's local storage)");
  }
  return validateProfile(parsed);
}

export function saveProfile(repository: string, profile: WorkingProfile, storage: Storage | null = defaultStorage()): void {
  const valid = validateProfile(profile);
  if (!storage) throw new Error("Local storage is unavailable in this browser context");
  try {
    storage.setItem(profileStorageKey(repository), JSON.stringify(valid));
  } catch (err) {
    throw new Error(`Failed to save working profile: ${err instanceof Error ? err.message : String(err)}`);
  }
}

export function clipUtf8(value: string, maxBytes: number, marker = "\n[truncated]"): string {
  const bytes = ENCODER.encode(value);
  if (bytes.length <= maxBytes) return value;
  const limit = Math.max(0, maxBytes - ENCODER.encode(marker).length);
  const slice = bytes.subarray(0, limit);
  let end = slice.length;
  while (end > 0 && (slice[end - 1] & 0xc0) === 0x80) end -= 1;
  if (end > 0 && (slice[end - 1] & 0x80) !== 0) end -= 1;
  return DECODER.decode(slice.subarray(0, end)) + marker;
}

export function formatProfile(instructions: ProfileInstruction[]): string {
  if (!instructions.length) return "";
  return [
    "Working style profile (user-owned; saved in this browser's local storage only):",
    ...instructions.map((instruction) => `- ${instruction.text} (source: ${instruction.source})`),
    "These are user guidance only; normal permissions and approvals still apply.",
  ].join("\n");
}

export function formatWorkflow(workflow: Workflow): string {
  const lines = [`Workflow: ${workflow.name}`];
  workflow.steps.forEach((step, index) => {
    lines.push(`Step ${index + 1}: purpose: ${step.purpose}; context: ${step.context}; agent (advisory, no routing): ${step.agent}; result: ${step.result}; evidence needed: ${step.evidenceNeeded}`);
  });
  const text = lines.join("\n");
  if (byteLen(text) > 4000) throw new Error(`Workflow "${workflow.name}" exceeds 4000 UTF-8 bytes; shorten it instead of truncating required decisions`);
  return text;
}

export function downloadProfile(content: string, filename: string, type: string): void {
  const url = URL.createObjectURL(new Blob([content], { type }));
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = filename;
  anchor.click();
  URL.revokeObjectURL(url);
}
