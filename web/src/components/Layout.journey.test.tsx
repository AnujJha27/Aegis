import { useState } from "react";
import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { api, type AgentRun, type FileComparison, type GitChange, type Task, type VerificationRun } from "../app/api";
import { Layout } from "./Layout";

vi.mock("@xterm/xterm", () => ({
  Terminal: class {
    loadAddon() {}
    open() {}
    onData() { return { dispose() {} }; }
    clear() {}
    write() {}
    focus() {}
    dispose() {}
  },
}));
vi.mock("@xterm/addon-fit", () => ({ FitAddon: class { fit() {} proposeDimensions() { return { cols: 80, rows: 24 }; } } }));
vi.mock("../features/code/monaco", () => ({}));
vi.mock("@monaco-editor/react", () => ({
  default: ({ value }: { value: string }) => <div data-testid="monaco-editor">{value}</div>,
  DiffEditor: () => <div data-testid="monaco-diff">Read-only diff</div>,
}));
vi.mock("../features/git/GitPanel", () => ({ GitPanel: () => <div>Git operations</div> }));

const task: Task = { id: "task-1", prompt: "Review the changed source", repository: "/repo", status: "active", createdAt: 1 };
const run: AgentRun = { id: "run-12345678", taskId: task.id, agent: "codex", status: "running", startedAt: 1, finishedAt: 0 };
const change: GitChange = { path: "source.cpp", old_path: null, index_status: " ", worktree_status: "M", additions: 1, deletions: 1, binary: false };
const verification: VerificationRun = { id: "verify-1", taskId: task.id, runId: run.id, command: ["ctest", "--test-dir", "build"], exitCode: 0, output: "all checks passed", startedAt: 2, finishedAt: 3 };
const comparison: FileComparison = {
  path: change.path, old_path: null, status: "M", binary: false, truncated: false,
  original: { source: "head", size: 12, exists: true, binary: false, truncated: false, content: "before" },
  modified: { source: "worktree", size: 11, exists: true, binary: false, truncated: false, content: "after" },
};

function Journey() {
  const [screen, setScreen] = useState<"session" | "review">("session");
  const [checked, setChecked] = useState(false);
  return <Layout
    repository={{ path: "/repo", branch: "main" }} tasks={[task]} selectedTask={task} onSelectTask={() => {}}
    taskPrompt="" onTaskPrompt={() => {}} onCreateTask={() => {}}
    agents={[{ name: "codex", available: true, structured: true, interactive: false, resumable: true, interruptible: true }]}
    selectedAgent="codex" onAgentChange={() => {}} onLaunch={() => {}} canLaunch
    run={run} runs={[run]} onSelectRun={() => {}} onDeleteRun={() => {}} onInterruptRun={() => {}} onTerminateRun={() => {}}
    actionsBusy={false} runFinished={false} turnBusy={false} turnCompleted turnInterrupted={false} onPtyInput={() => {}} onPtyResize={() => {}}
    ptyConnection="idle" events={[]} activityEvents={[]} prompt="" onPrompt={() => {}} onSend={() => {}} busy="Ready" sending={false}
    screen={screen} onScreen={setScreen} connection="connected" drawer="review" onDrawer={() => {}}
    gitChanges={[change]} gitStatus={{ repository: { path: "/repo" }, files: [change], branches: ["main"], current_branch: "main", clean: false, agent_running: true }}
    onGitChanged={async () => {}} verification={checked ? verification : undefined} onVerify={() => setChecked(true)}
    provenance={[]} error="" onDismissError={() => {}} onOpenFile={() => {}} resumable />;
}

afterEach(() => { cleanup(); vi.restoreAllMocks(); });

describe("workspace UI journey", () => {
  it("moves from Session through Review and Diff to verification evidence", async () => {
    vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} });
    vi.spyOn(api, "findings").mockResolvedValue([]);
    vi.spyOn(api, "compareFiles").mockResolvedValue(comparison);
    render(<Journey />);

    expect(screen.getByRole("heading", { name: "Review the changed source" })).toBeTruthy();
    expect(screen.getByText(/TURN COMPLETE · RUN ACTIVE/)).toBeTruthy();
    fireEvent.click(screen.getByRole("button", { name: "Review" }));
    await screen.findByText("CHANGED FILES", {}, { timeout: 5000 });
    fireEvent.click(await screen.findByRole("button", { name: /source\.cpp/ }));
    expect((await screen.findByTestId("monaco-diff")).textContent).toBe("Read-only diff");
    expect(screen.getByText("HEAD ↔ WORKTREE")).toBeTruthy();

    fireEvent.click(screen.getByRole("button", { name: "Run check" }));
    await waitFor(() => expect(screen.getByText("Verification passed")).toBeTruthy());
    expect(screen.getByText("all checks passed")).toBeTruthy();
  });
});
