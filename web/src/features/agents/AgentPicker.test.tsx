import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import type { AgentRun } from "../../app/api";
import { AgentPicker } from "./AgentPicker";

const run: AgentRun = { id: "run-1", taskId: "task-1", agent: "codex", status: "failed", startedAt: 1, finishedAt: 2 };

afterEach(cleanup);

describe("run deletion confirmation", () => {
  it("requires an in-app confirmation before deleting a run", () => {
    const onDeleteRun = vi.fn();
    render(<AgentPicker agents={[]} selected="codex" onChange={vi.fn()} onLaunch={vi.fn()} runs={[run]} run={run} runFinished onSelectRun={vi.fn()} onDeleteRun={onDeleteRun} />);

    fireEvent.click(screen.getByRole("button", { name: "Delete selected run" }));
    expect(screen.getByRole("group", { name: "Confirm run deletion" })).toBeTruthy();
    expect(onDeleteRun).not.toHaveBeenCalled();

    fireEvent.click(screen.getByRole("button", { name: "Cancel" }));
    expect(screen.queryByRole("group", { name: "Confirm run deletion" })).toBeNull();
    expect(onDeleteRun).not.toHaveBeenCalled();

    fireEvent.click(screen.getByRole("button", { name: "Delete selected run" }));
    fireEvent.click(screen.getByRole("button", { name: "Delete run" }));
    expect(onDeleteRun).toHaveBeenCalledWith(run.id);
  });
});
