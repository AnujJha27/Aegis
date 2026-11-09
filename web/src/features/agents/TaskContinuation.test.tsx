import { act, cleanup, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { Agent, AgentRun, HandoffContext, Task } from "../../app/api";
import { api } from "../../app/api";
import { formatHandoff } from "../review/handoffText";
import { TaskContinuation } from "./TaskContinuation";

vi.mock("../../app/api", () => ({
  api: { updateTaskContext: vi.fn(), handoff: vi.fn(), continueTask: vi.fn() },
}));

const apiMock = vi.mocked(api, true);

const agents: Agent[] = [
  { name: "codex", available: true, structured: false, interactive: false, resumable: false, interruptible: false },
  { name: "opencode", available: true, structured: true, interactive: false, resumable: true, interruptible: false },
  { name: "claude", available: false, structured: true, interactive: true, resumable: true, interruptible: true },
];

const task = {
  id: "task-1",
  prompt: "Refactor auth module · 認証モジュールのリファクタリング",
  repository: "repo",
  status: "open",
  createdAt: 1,
  context: {
    goal: "Ship SSO login",
    constraints: "Keine Breaking Changes",
    decisions: "JWT verwenden",
    attempts: "Tried middleware approach",
    unfinished: "Refresh token rotation",
    next_action: "续写失败用例",
  },
} as unknown as Task;

const taskWithoutContext = { id: "task-2", prompt: "Other task", repository: "repo", status: "open", createdAt: 2 } as unknown as Task;

const sourceRun: AgentRun = { id: "run-1", taskId: "task-1", agent: "codex", status: "completed", startedAt: 1, finishedAt: 2 };
const otherSourceRun: AgentRun = { id: "run-2", taskId: "task-1", agent: "codex", status: "completed", startedAt: 3, finishedAt: 4 };
const continuedRun: AgentRun = { id: "run-3", taskId: "task-1", agent: "opencode", status: "running", startedAt: 5, finishedAt: 0 };

const handoffContext: HandoffContext = {
  task_id: "task-1",
  prompt: "Refactor auth module · 認証モジュールのリファクタリング",
  recent_events: [],
  diff: "--- a/src/auth.ts\n+++ b/src/auth.ts\n",
  changed_files: ["src/auth.ts"],
  changed_files_truncated: false,
  verification: null,
  findings: [],
};

const onTaskUpdated = vi.fn();
const onContinued = vi.fn();

function renderComponent(overrides: Record<string, unknown> = {}) {
  return render(
    <TaskContinuation
      task={task}
      run={sourceRun}
      agents={agents}
      sourceBusy={false}
      onTaskUpdated={onTaskUpdated}
      onContinued={onContinued}
      {...overrides}
    />,
  );
}

function openContextEditor() {
  fireEvent.click(screen.getByText("Task context"));
}

beforeEach(() => {
  vi.clearAllMocks();
  apiMock.handoff.mockResolvedValue(handoffContext);
  apiMock.continueTask.mockResolvedValue(continuedRun);
  apiMock.updateTaskContext.mockResolvedValue(task as never);
});

afterEach(() => {
  cleanup();
});

describe("TaskContinuation", () => {
  it("does not start handoff for a new source while a save is pending", async () => {
    let resolveSave!: (value: Task) => void;
    apiMock.updateTaskContext.mockImplementationOnce(
      () => new Promise<Task>((resolve) => { resolveSave = resolve; }),
    );
    const view = renderComponent();
    fireEvent.click(screen.getByRole("button", { name: /Continue with another agent/i }));
    await waitFor(() => expect(apiMock.updateTaskContext).toHaveBeenCalledTimes(1));
    await act(async () => {
      view.rerender(
        <TaskContinuation task={task} run={otherSourceRun} agents={agents} sourceBusy={false} onTaskUpdated={onTaskUpdated} onContinued={onContinued} />,
      );
    });
    await act(async () => {
      resolveSave(task as never);
    });
    expect(apiMock.handoff).not.toHaveBeenCalled();
    expect(screen.queryByRole("dialog", { name: "Continue with another agent" })).toBeNull();
    expect(onContinued).not.toHaveBeenCalled();
  });

  it("does not call onContinued when unmounted during a pending confirmation", async () => {
    let resolveContinue!: (value: AgentRun) => void;
    apiMock.continueTask.mockImplementationOnce(
      () => new Promise<AgentRun>((resolve) => { resolveContinue = resolve; }),
    );
    const view = renderComponent();
    fireEvent.click(screen.getByRole("button", { name: /Continue with another agent/i }));
    const dialog = await screen.findByRole("dialog", { name: "Continue with another agent" });
    fireEvent.click(within(dialog).getByRole("button", { name: "Continue task" }));
    await waitFor(() => expect(apiMock.continueTask).toHaveBeenCalledTimes(1));
    act(() => view.unmount());
    await act(async () => {
      resolveContinue(continuedRun);
    });
    expect(onContinued).not.toHaveBeenCalled();
  });

  it("keeps unsaved context drafts when the source run switches", () => {
    const view = renderComponent();
    openContextEditor();
    fireEvent.change(screen.getByLabelText("Goal"), { target: { value: "unsaved-draft" } });
    view.rerender(
      <TaskContinuation task={task} run={otherSourceRun} agents={agents} sourceBusy={false} onTaskUpdated={onTaskUpdated} onContinued={onContinued} />,
    );
    expect((screen.getByLabelText("Goal") as HTMLInputElement).value).toBe("unsaved-draft");
  });

  it("saves edited task context via api.updateTaskContext", async () => {
    renderComponent();
    openContextEditor();
    fireEvent.change(screen.getByLabelText("Goal"), { target: { value: "Ship SSO login · シングルサインオン" } });
    fireEvent.click(screen.getByRole("button", { name: "Save task context" }));
    await waitFor(() =>
      expect(apiMock.updateTaskContext).toHaveBeenCalledWith(
        "task-1",
        expect.objectContaining({ goal: "Ship SSO login · シングルサインオン", next_action: "续写失败用例" }),
      ),
    );
    await waitFor(() => expect(onTaskUpdated).toHaveBeenCalled());
  });

  it("keeps the draft when saving fails and rejects oversized or NUL input", async () => {
    apiMock.updateTaskContext.mockRejectedValueOnce(new Error("save failed"));
    renderComponent();
    openContextEditor();
    fireEvent.change(screen.getByLabelText("Goal"), { target: { value: "draft-α" } });
    fireEvent.click(screen.getByRole("button", { name: "Save task context" }));
    expect((await screen.findByRole("alert")).textContent).toContain("save failed");
    expect((screen.getByLabelText("Goal") as HTMLInputElement).value).toBe("draft-α");

    fireEvent.change(screen.getByLabelText("Goal"), { target: { value: "é".repeat(2001) } });
    fireEvent.click(screen.getByRole("button", { name: "Save task context" }));
    expect(apiMock.updateTaskContext).toHaveBeenCalledTimes(1);

    fireEvent.change(screen.getByLabelText("Goal"), { target: { value: "bad\u0000value" } });
    fireEvent.click(screen.getByRole("button", { name: "Save task context" }));
    expect(apiMock.updateTaskContext).toHaveBeenCalledTimes(1);
    expect((screen.getByLabelText("Goal") as HTMLInputElement).value).toBe("bad\u0000value");
  });

  it("blocks continuation while the source run is busy but still allows saving context", async () => {
    renderComponent({ sourceBusy: true });
    const continueButton = screen.getByRole("button", { name: /Continue with another agent/i }) as HTMLButtonElement;
    expect(continueButton.disabled).toBe(true);
    fireEvent.click(continueButton);
    expect(apiMock.handoff).not.toHaveBeenCalled();

    openContextEditor();
    fireEvent.change(screen.getByLabelText("Goal"), { target: { value: "busy-save" } });
    fireEvent.click(screen.getByRole("button", { name: "Save task context" }));
    await waitFor(() => expect(apiMock.updateTaskContext).toHaveBeenCalled());
  });

  it("saves the draft, loads handoff, previews the exact handoff text, and confirms with it", async () => {
    renderComponent();
    openContextEditor();
    fireEvent.change(screen.getByLabelText("Next action"), { target: { value: "Fertigstellen der Migration" } });
    fireEvent.click(screen.getByRole("button", { name: /Continue with another agent/i }));
    await waitFor(() => expect(apiMock.updateTaskContext).toHaveBeenCalled());

    const dialog = await screen.findByRole("dialog", { name: "Continue with another agent" });
    const select = within(dialog).getByLabelText("Continue with") as HTMLSelectElement;
    expect(Array.from(select.querySelectorAll("option")).map((option) => option.value)).toEqual(["opencode"]);

    const preview = within(dialog).getByLabelText("Handoff preview") as HTMLTextAreaElement;
    const expected = formatHandoff(handoffContext, sourceRun);
    expect(preview.readOnly).toBe(true);
    expect(preview.value).toBe(expected);
    expect(apiMock.continueTask).not.toHaveBeenCalled();

    fireEvent.click(within(dialog).getByRole("button", { name: "Continue task" }));
    await waitFor(() => expect(apiMock.continueTask).toHaveBeenCalledWith("task-1", "opencode", "run-1", expected));
    await waitFor(() => expect(onContinued).toHaveBeenCalledWith(continuedRun, "run-1"));
  });

  it("does not open a stale handoff dialog when the source run changes before load resolves", async () => {
    let resolveHandoff!: (value: HandoffContext) => void;
    apiMock.handoff.mockImplementation(
      () => new Promise<HandoffContext>((resolve) => { resolveHandoff = resolve; }),
    );
    const view = renderComponent();
    fireEvent.click(screen.getByRole("button", { name: /Continue with another agent/i }));
    await waitFor(() => expect(apiMock.handoff).toHaveBeenCalledTimes(1));
    await act(async () => {
      view.rerender(
        <TaskContinuation task={task} run={otherSourceRun} agents={agents} sourceBusy={false} onTaskUpdated={onTaskUpdated} onContinued={onContinued} />,
      );
    });
    await act(async () => {
      resolveHandoff(handoffContext);
    });
    expect(screen.queryByRole("dialog", { name: "Continue with another agent" })).toBeNull();
    expect(onContinued).not.toHaveBeenCalled();
  });

  it("does not call onContinued for a task the user switched away from", async () => {
    let resolveContinue!: (value: AgentRun) => void;
    apiMock.continueTask.mockImplementation(
      () => new Promise<AgentRun>((resolve) => { resolveContinue = resolve; }),
    );
    const view = renderComponent();
    fireEvent.click(screen.getByRole("button", { name: /Continue with another agent/i }));
    const dialog = await screen.findByRole("dialog", { name: "Continue with another agent" });
    fireEvent.click(within(dialog).getByRole("button", { name: "Continue task" }));
    await waitFor(() => expect(apiMock.continueTask).toHaveBeenCalledTimes(1));

    await act(async () => {
      view.rerender(
        <TaskContinuation task={taskWithoutContext} run={sourceRun} agents={agents} sourceBusy={false} onTaskUpdated={onTaskUpdated} onContinued={onContinued} />,
      );
    });
    await act(async () => {
      resolveContinue(continuedRun);
    });
    expect(onContinued).not.toHaveBeenCalled();
  });
});
