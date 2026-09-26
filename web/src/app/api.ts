export type Repository = {
  path: string;
  branch?: string;
  files?: number;
  insertions?: number;
  deletions?: number;
  exists?: boolean;
};

export type GitChange = { path: string; index_status: string; worktree_status: string };
export type GitStatus = { repository: Repository; files: GitChange[]; branches: string[]; current_branch: string; clean: boolean; agent_running: boolean; output?: string };

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

const task = (value: Record<string, unknown>): Task => ({ id: String(value.id), prompt: String(value.prompt), repository: String(value.repository), status: String(value.status), createdAt: Number(value.created_at ?? value.createdAt ?? 0) });
const run = (value: Record<string, unknown>): AgentRun => ({ id: String(value.id), taskId: String(value.task_id ?? value.taskId), agent: String(value.agent), status: String(value.status), startedAt: Number(value.started_at ?? value.startedAt ?? 0), finishedAt: Number(value.finished_at ?? value.finishedAt ?? 0) });
export const agentEvent = (value: Record<string, unknown>): AgentEvent => ({ id: String(value.id), taskId: String(value.task_id ?? value.taskId), runId: String(value.run_id ?? value.runId), type: String(value.type), agent: String(value.agent), content: String(value.content ?? ""), timestamp: Number(value.timestamp ?? 0) });
const verification = (value: Record<string, unknown>): VerificationRun => ({ id: String(value.id), command: String(value.command), exitCode: Number(value.exit_code ?? value.exitCode), output: String(value.output ?? ""), startedAt: Number(value.started_at ?? value.startedAt), finishedAt: Number(value.finished_at ?? value.finishedAt) });

export const api = {
  repository: () => request<Repository>("/api/repository"),
  tasks: async () => (await request<Record<string, unknown>[]>("/api/tasks")).map(task),
  agents: () => request<Agent[]>("/api/agents"),
  events: async (taskId: string) => (await request<Record<string, unknown>[]>(`/api/events?task_id=${encodeURIComponent(taskId)}`)).map(agentEvent),
  changes: () => request<{ diff: string }>("/api/changes"),
  gitStatus: () => request<GitStatus>("/api/git/status"),
  stage: (path: string) => request<GitStatus>("/api/git/stage", { method: "POST", body: JSON.stringify({ path }) }),
  unstage: (path: string) => request<GitStatus>("/api/git/unstage", { method: "POST", body: JSON.stringify({ path }) }),
  commit: (message: string) => request<GitStatus>("/api/git/commit", { method: "POST", body: JSON.stringify({ message }) }),
  switchBranch: (branch: string) => request<GitStatus>("/api/git/branch", { method: "POST", body: JSON.stringify({ branch }) }),
  pull: () => request<GitStatus>("/api/git/pull", { method: "POST", body: "{}" }),
  push: () => request<GitStatus>("/api/git/push", { method: "POST", body: "{}" }),
  runs: async (taskId: string) => (await request<Record<string, unknown>[]>(`/api/tasks/${taskId}/runs`)).map(run),
  handoff: async (taskId: string) => {
    const context = await request<Omit<HandoffContext, "recent_events"> & { recent_events: Record<string, unknown>[] }>(`/api/tasks/${taskId}/handoff`);
    return { ...context, recent_events: context.recent_events.map(agentEvent) };
  },
  graph: (taskId: string) => request<TaskGraph>(`/api/tasks/${taskId}/graph`),
  provenance: (taskId: string) => request<{ task_id: string; records: ProvenanceRecord[] }>(`/api/tasks/${taskId}/provenance`),
  createTask: async (prompt: string) => task(await request<Record<string, unknown>>("/api/tasks", { method: "POST", body: JSON.stringify({ prompt }) })),
  launch: async (taskId: string, agent: string) => run(await request<Record<string, unknown>>(`/api/tasks/${taskId}/runs`, { method: "POST", body: JSON.stringify({ agent }) })),
  send: (runId: string, message: string) => request<{ status: string }>(`/api/runs/${runId}/messages`, { method: "POST", body: JSON.stringify({ message }) }),
  verify: async (command: string[]) => verification(await request<Record<string, unknown>>("/api/verify", { method: "POST", body: JSON.stringify({ command }) })),
};
