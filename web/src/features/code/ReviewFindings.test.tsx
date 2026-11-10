import { act, cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { api, type AgentRun, type ReviewFinding } from "../../app/api";
import { ReviewFindings } from "./ReviewFindings";

const run1: AgentRun = { id: "run1", taskId: "task1", agent: "codex", status: "running", startedAt: 1, finishedAt: 0, turnStatus: "turn.completed" };
const f1: ReviewFinding = { id: "f1", task_id: "task1", run_id: "run1", file_path: "a.ts", start_line: 1, end_line: null, message: "Fix the bug", status: "open", created_at: 2, updated_at: 2 };
const f2: ReviewFinding = { ...f1, id: "f2", task_id: "task2", message: "Fix other bug" };

async function openAndPreview() {
  const view = render(<ReviewFindings taskId="task1" activeRun={run1} activeRunId="run1" filePath="a.ts" />);
  await screen.findByText("Fix the bug");
  fireEvent.click(screen.getByRole("checkbox", { name: "Select finding f1" }));
  fireEvent.click(screen.getByRole("button", { name: "Review revision request" }));
  return view;
}

beforeEach(() => {
  vi.spyOn(api, "findings").mockImplementation((taskId) => Promise.resolve(taskId === "task1" ? [f1] : [f2]));
  vi.spyOn(api, "send").mockResolvedValue({ status: "ok" });
  vi.spyOn(api, "updateFindingStatus").mockResolvedValue(undefined);
});
afterEach(() => { cleanup(); vi.restoreAllMocks(); });

it("previews exact selected snapshot and sends without touching finding statuses", async () => {
  await openAndPreview();
  const dialog = screen.getByRole("dialog", { name: "Revision request preview" });
  expect(dialog.querySelector("pre")?.textContent).toContain("Fix the bug");
  expect(dialog.querySelector("pre")?.textContent).toContain("(finding f1)");
  fireEvent.click(screen.getByRole("button", { name: "Send for revision" }));
  await waitFor(() => expect(api.send).toHaveBeenCalledWith("run1", expect.stringContaining("(finding f1)")));
  expect(api.updateFindingStatus).not.toHaveBeenCalled();
  await waitFor(() => expect(screen.queryByRole("dialog", { name: "Revision request preview" })).toBeNull());
});

it("cannot send when agent is not codex", async () => {
  vi.spyOn(api, "findings").mockResolvedValue([f1]);
  render(<ReviewFindings taskId="task1" activeRun={{ ...run1, agent: "shell" }} activeRunId="run1" filePath="a.ts" />);
  await screen.findByText("Fix the bug");
  expect((screen.getByRole("button", { name: "Review revision request" }) as HTMLButtonElement).disabled).toBe(true);
  expect(screen.getByRole("note").textContent).toContain("only codex");
});

it("keeps preview and selection when send is rejected", async () => {
  vi.spyOn(api, "send").mockRejectedValue(new Error("send failed"));
  await openAndPreview();
  fireEvent.click(screen.getByRole("button", { name: "Send for revision" }));
  await screen.findByText("send failed");
  expect(screen.getByRole("dialog", { name: "Revision request preview" })).toBeDefined();
  expect((screen.getByRole("checkbox", { name: "Select finding f1" }) as HTMLInputElement).checked).toBe(true);
});

it("does not clear a pending task1 send result into task2 state", async () => {
  let resolveSend!: (value: { status: string }) => void;
  vi.spyOn(api, "send").mockReturnValue(new Promise((resolve) => { resolveSend = resolve; }));
  const view = await openAndPreview();
  fireEvent.click(screen.getByRole("button", { name: "Send for revision" }));
  await waitFor(() => expect(api.send).toHaveBeenCalled());
  await act(async () => {
    view.rerender(<ReviewFindings taskId="task2" activeRun={run1} activeRunId="run1" filePath="a.ts" />);
  });
  await screen.findByText("Fix other bug");
  await act(async () => {
    resolveSend({ status: "ok" });
  });
  await waitFor(() => expect(api.updateFindingStatus).not.toHaveBeenCalled());
  expect(screen.queryByRole("dialog", { name: "Revision request preview" })).toBeNull();
  expect(screen.queryByRole("checkbox", { name: "Select finding f1" })).toBeNull();
  expect(api.updateFindingStatus).not.toHaveBeenCalled();
});

it("marks an open finding not applicable with a required explanation", async () => {
  const view = render(<ReviewFindings taskId="task1" activeRun={run1} activeRunId="run1" filePath="a.ts" />);
  await screen.findByText("Fix the bug");
  fireEvent.click(screen.getByRole("button", { name: "Mark finding not applicable" }));
  const confirm = screen.getByRole("button", { name: "Confirm not applicable" }) as HTMLButtonElement;
  expect(confirm.disabled).toBe(true);
  fireEvent.change(screen.getByLabelText("Not applicable explanation"), { target: { value: "Handled upstream" } });
  fireEvent.click(screen.getByRole("button", { name: "Confirm not applicable" }));
  await waitFor(() => expect(api.updateFindingStatus).toHaveBeenCalledWith("f1", "not_applicable", "Handled upstream"));
  await screen.findByText("Not applicable: Handled upstream");
  expect(view.container.querySelector("li.not_applicable")).toBeTruthy();
  expect(screen.queryByRole("button", { name: "Confirm not applicable" })).toBeNull();
});

it("blocks an empty not applicable explanation", async () => {
  render(<ReviewFindings taskId="task1" activeRun={run1} activeRunId="run1" filePath="a.ts" />);
  await screen.findByText("Fix the bug");
  fireEvent.click(screen.getByRole("button", { name: "Mark finding not applicable" }));
  fireEvent.change(screen.getByLabelText("Not applicable explanation"), { target: { value: "   " } });
  expect((screen.getByRole("button", { name: "Confirm not applicable" }) as HTMLButtonElement).disabled).toBe(true);
  expect(api.updateFindingStatus).not.toHaveBeenCalled();
});

it("shows the recorded explanation and reopens a not applicable finding", async () => {
  vi.spyOn(api, "findings").mockResolvedValue([{ ...f1, status: "not_applicable", disposition_reason: "Handled upstream" }]);
  const view = render(<ReviewFindings taskId="task1" activeRun={run1} activeRunId="run1" filePath="a.ts" />);
  await screen.findByText("Not applicable: Handled upstream");
  fireEvent.click(screen.getByRole("button", { name: "Reopen finding" }));
  await waitFor(() => expect(api.updateFindingStatus).toHaveBeenCalledWith("f1", "open"));
  await waitFor(() => expect(view.container.querySelector("li.not_applicable")).toBeNull());
});