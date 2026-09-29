import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import type { AgentRun } from "../../app/api";
import { AgentPicker } from "./AgentPicker";

const run: AgentRun = { id: "run-1", taskId: "task-1", agent: "codex", status: "failed", startedAt: 1, finishedAt: 2 };

afterEach(cleanup);

describe("run deletion confirmation", () => {
  it("requires an in-app confirmation before deleting a run", () => {
    const onDeleteRun = vi.fn();
    render(<AgentPicker agents={[]} selected="codex" onChange={vi.fn()} onLaunch={vi.fn()} canLaunch={false} runs={[run]} run={run} runFinished turnBusy={false} actionsBusy={false} onSelectRun={vi.fn()} onInterruptRun={vi.fn()} onTerminateRun={vi.fn()} onDeleteRun={onDeleteRun} />);

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

describe("run controls", () => {
  const codex = { name: "codex", available: true, structured: true, interactive: false, resumable: true, interruptible: true };

  it("interrupts a busy supported run and confirms stopping it", () => {
    const onInterruptRun = vi.fn();
    const onTerminateRun = vi.fn();
    render(<AgentPicker agents={[codex]} selected="codex" onChange={vi.fn()} onLaunch={vi.fn()} canLaunch runs={[{ ...run, status: "running" }]} run={{ ...run, status: "running" }} runFinished={false} turnBusy actionsBusy={false} onInterruptRun={onInterruptRun} onTerminateRun={onTerminateRun} onSelectRun={vi.fn()} onDeleteRun={vi.fn()} />);

    fireEvent.click(screen.getByRole("button", { name: "Interrupt turn" }));
    expect(onInterruptRun).toHaveBeenCalledWith(run.id);
    fireEvent.click(screen.getByRole("button", { name: "Stop run" }));
    expect(screen.getByRole("group", { name: "Confirm agent stop" })).toBeTruthy();
    expect(onTerminateRun).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole("button", { name: "Confirm stop" }));
    expect(onTerminateRun).toHaveBeenCalledWith(run.id);
  });

  it("disables interrupt when the supported adapter has no active turn", () => {
    render(<AgentPicker agents={[codex]} selected="codex" onChange={vi.fn()} onLaunch={vi.fn()} canLaunch runs={[{ ...run, status: "running" }]} run={{ ...run, status: "running" }} runFinished={false} turnBusy={false} actionsBusy={false} onInterruptRun={vi.fn()} onTerminateRun={vi.fn()} onSelectRun={vi.fn()} onDeleteRun={vi.fn()} />);
    expect(screen.getByRole("button", { name: "Interrupt turn" }).hasAttribute("disabled")).toBe(true);
  });
});
