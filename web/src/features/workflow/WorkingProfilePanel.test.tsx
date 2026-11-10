import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { Task } from "../../app/api";
import { WorkingProfile } from "./WorkingProfilePanel";
import type { Workflow, WorkflowStep } from "./workingProfile";

vi.mock("../../app/api", () => ({
  api: { workingProfile: vi.fn(), saveWorkingProfile: vi.fn() },
}));

import { api } from "../../app/api";
const apiMock = vi.mocked(api, true);

const makeTask = (): Task =>
  ({
    id: "task-1",
    repository: "repo/a",
    prompt: "task 1",
    status: "open",
    createdAt: 1704067200000,
    context: { goal: "g", constraints: "", decisions: "", attempts: "", unfinished: "", next_action: "" },
  });

const step: WorkflowStep = {
  purpose: "step one",
  context: "ctx",
  agent: "codex",
  result: "done",
  evidenceNeeded: "tests",
};
const workflow: Workflow = { name: "w1", steps: [step] } as unknown as Workflow;
const workflowsJson = JSON.stringify({ workflows: [{ name: "w1", steps: [step] }] });

describe("WorkingProfile", () => {
  beforeEach(() => {
    vi.clearAllMocks();
    apiMock.workingProfile.mockResolvedValue({ version: 1, revision: 1, instructions: [], workflows: [] } as never);
    apiMock.saveWorkingProfile.mockResolvedValue({ version: 1, revision: 2, instructions: [], workflows: [] } as never);
  });

  afterEach(() => { cleanup(); localStorage.clear(); vi.restoreAllMocks(); });

  it("applies the exact workflow without saving the profile", async () => {
    const onApplyWorkflow = vi.fn();
    render(<WorkingProfile task={makeTask()} onApplyWorkflow={onApplyWorkflow} />);
    await waitFor(() => expect((screen.getByLabelText("workflow") as HTMLTextAreaElement).disabled).toBe(false));
    fireEvent.click(screen.getByText("Working style"));
    fireEvent.click(screen.getByText("Reusable workflows"));
    fireEvent.change(screen.getByLabelText("workflow") as HTMLTextAreaElement, { target: { value: workflowsJson } });
    fireEvent.change(screen.getByRole("combobox") as HTMLSelectElement, { target: { value: "w1" } });
    fireEvent.click(screen.getByRole("button", { name: "apply workflow" }));
    expect(onApplyWorkflow).toHaveBeenCalledTimes(1);
    expect(onApplyWorkflow).toHaveBeenCalledWith(workflow);
    expect(apiMock.saveWorkingProfile).not.toHaveBeenCalled();
  });
});