export type Repository = {
  path: string;
  branch?: string;
  files?: number;
  insertions?: number;
  deletions?: number;
  exists?: boolean;
};

export type Task = {
  id: string;
  prompt: string;
  repository: string;
  status: string;
  createdAt: number;
};

export type Agent = {
  name: string;
  available: boolean;
  structured: boolean;
  interactive: boolean;
};

export type AgentRun = {
  id: string;
  taskId: string;
  agent: string;
  status: string;
  startedAt: number;
  finishedAt: number;
};

export type AgentEvent = {
  id: string;
  taskId: string;
  runId: string;
  type: string;
  agent: string;
  content: string;
  timestamp: number;
};

export type VerificationRun = {
  id: string;
  command: string;
  exitCode: number;
  output: string;
  startedAt: number;
  finishedAt: number;
};

export type HandoffContext = {
  task_id: string;
  prompt: string;
  recent_events: AgentEvent[];
  diff: string;
  changed_files: string[];
  verification: VerificationRun | null;
};

export type GraphNode = { id: string; type: string; label: string };
export type GraphEdge = { from: string; to: string };
export type TaskGraph = { task_id: string; nodes: GraphNode[]; edges: GraphEdge[] };
export type ProvenanceRecord = {
  event_id: string;
  task_id: string;
  run_id: string;
  agent: string;
  event_type: string;
  timestamp: number;
  attribution: string;
  changed_files: string[];
};

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(path, {
    ...init,
    headers: { "Content-Type": "application/json", ...(init?.headers ?? {}) },
  });
  const body = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(body.error?.message ?? `Request failed (${response.status})`);
  return body as T;
}

export const api = {
  repository: () => request<Repository>("/api/repository"),
  tasks: () => request<Task[]>("/api/tasks"),
  agents: () => request<Agent[]>("/api/agents"),
  events: (taskId: string) => request<AgentEvent[]>(`/api/events?task_id=${encodeURIComponent(taskId)}`),
  changes: () => request<{ diff: string }>("/api/changes"),
  handoff: (taskId: string) => request<HandoffContext>(`/api/tasks/${taskId}/handoff`),
  graph: (taskId: string) => request<TaskGraph>(`/api/tasks/${taskId}/graph`),
  provenance: (taskId: string) => request<{ task_id: string; records: ProvenanceRecord[] }>(`/api/tasks/${taskId}/provenance`),
  createTask: (prompt: string) => request<Task>("/api/tasks", { method: "POST", body: JSON.stringify({ prompt }) }),
  launch: (taskId: string, agent: string) => request<AgentRun>(`/api/tasks/${taskId}/runs`, { method: "POST", body: JSON.stringify({ agent }) }),
  send: (runId: string, message: string) => request<{ status: string }>(`/api/runs/${runId}/messages`, { method: "POST", body: JSON.stringify({ message }) }),
  verify: (command: string[]) => request<VerificationRun>("/api/verify", { method: "POST", body: JSON.stringify({ command }) }),
};
