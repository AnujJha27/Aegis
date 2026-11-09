import { cleanup, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it } from "vitest";
import type { AgentEvent, Task } from "../../app/api";
import { AgentSession } from "./AgentSession";

const baseTask: Task = { id: "task-1", prompt: "Review the changed source", repository: "/repo", status: "active", createdAt: 1 };

function renderSession(task?: Task, events: AgentEvent[] = []) {
  return render(<AgentSession task={task} runFinished={false} turnBusy={false} turnCompleted={false} turnInterrupted={false} resumable={false} interactive={false} ptyConnection="connected" onPtyInput={() => {}} onPtyResize={() => {}} events={events} prompt="" onPrompt={() => {}} onSend={() => {}} busy="Ready" sending={false}>{null}</AgentSession>);
}

afterEach(cleanup);

describe("AgentSession task heading", () => {
  it("shows a short prompt unchanged", () => {
    renderSession(baseTask);
    expect(screen.getByRole("heading", { name: "Review the changed source" })).toBeTruthy();
  });

  it("truncates a long multiline prompt to its first line with ellipsis and keeps the full original request reachable", () => {
    const first = "Fix the parser so that a deeply nested expression no longer drops trailing tokens";
    const task = { ...baseTask, prompt: `${first}\nsecond line of the request with much more detail that must remain accessible` } as Task;
    renderSession(task);
    expect(screen.getByRole("heading", { name: `${first.slice(0, 80)}…` })).toBeTruthy();
    const details = screen.getByText("Original request").closest("details");
    expect(details).toBeTruthy();
    expect((details as HTMLElement).textContent).toContain(task.prompt);
  });

  it("renders an imported message card with role, source reference, and historical label without executing content", () => {
    const event: AgentEvent = {
      id: "import-1",
      taskId: "task-1",
      runId: "import-run",
      type: "import.message",
      agent: "import",
      timestamp: 2,
      content: JSON.stringify({ role: "user", text: "Check the parser before the eval step", source_name: "src/parser.ts", source_line: 12, historical: true }),
    };
    renderSession(baseTask, [event]);
    const pre = screen.getByText((_, element) => element?.tagName === "PRE" && element.textContent?.includes("[imported user"));
    expect(pre).toBeTruthy();
    expect(pre?.textContent).toContain("src/parser.ts:12");
    expect(pre?.textContent).toContain("(historical)");
    expect(pre?.textContent).toContain("Check the parser before the eval step");
    expect(screen.queryByText("eval")).toBeNull();
  });

  it("shows the task next action near the heading when provided", () => {
    const task = { ...baseTask, context: { next_action: "Run the failing suite" } } as Task;
    renderSession(task);
    expect(screen.getByText("Run the failing suite")).toBeTruthy();
  });
});
