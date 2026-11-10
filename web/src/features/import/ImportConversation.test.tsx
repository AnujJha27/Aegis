import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { api } from "../../app/api";
import { profileStorageKey } from "../workflow/workingProfile";
import { ImportConversation } from "./ImportConversation";

const lines = [
  JSON.stringify({ type: "session_meta", payload: { id: "s1" } }),
  JSON.stringify({ type: "response_item", payload: { id: "u1", type: "message", role: "user", content: [{ type: "input_text", text: "Do the thing" }] } }),
].join("\n");

function fakeFile(): File {
  return { name: "log.jsonl", size: lines.length, text: async () => lines } as unknown as File;
}

describe("ImportConversation", () => {
  afterEach(() => { cleanup(); vi.restoreAllMocks(); });

  it("preserves edited goal when import API fails", async () => {
    vi.spyOn(api, "importCodex").mockRejectedValue(new Error("Import failed"));
    render(<ImportConversation />);
    const input = screen.getByLabelText("Codex JSONL log file") as HTMLInputElement;
    fireEvent.change(input, { target: { files: [fakeFile()] } });
    const goal = await screen.findByLabelText("goal");
    fireEvent.change(goal, { target: { value: "Custom goal" } });
    fireEvent.click(screen.getByRole("button", { name: "Import task" }));
    await screen.findByText("Import failed");
    expect((screen.getByDisplayValue("Custom goal") as HTMLTextAreaElement).value).toBe("Custom goal");
    expect(api.importCodex).toHaveBeenCalledWith(
      expect.objectContaining({
        session_id: "s1",
        prompt: "Do the thing",
        context: expect.objectContaining({ goal: "Custom goal" }),
      }),
    );
  });

  it("does not retain a profile after parse until Retain is clicked", async () => {
    localStorage.clear();
    vi.spyOn(api, "importCodex").mockResolvedValue({ id: "task-new" } as never);
    vi.spyOn(api, "workingProfile").mockResolvedValue({ version: 1, revision: 1, instructions: [], workflows: [] } as never);
    const saveProfile = vi.spyOn(api, "saveWorkingProfile").mockResolvedValue({ version: 1, revision: 2, instructions: [], workflows: [] } as never);
    render(<ImportConversation repository="/repo" />);
    const input = screen.getByLabelText("Codex JSONL log file") as HTMLInputElement;
    fireEvent.change(input, { target: { files: [fakeFile()] } });
    await screen.findByLabelText("goal");
    const key = profileStorageKey("/repo");
    expect(localStorage.getItem(key)).toBeNull();

    fireEvent.click(screen.getByRole("button", { name: "Retain as working instruction" }));
    await screen.findByText("Retained as working instruction", { exact: false });
    expect(saveProfile).toHaveBeenCalledTimes(1);
    const saved = saveProfile.mock.calls[0][0];
    expect(saved.instructions).toHaveLength(1);
    expect(saved.instructions[0].text).toBe("Do the thing");
    expect(saved.instructions[0].source).toContain("s1");
    expect(saved.instructions[0].source).toContain("log.jsonl:2");
    expect(localStorage.getItem(key)).toBeNull();
    localStorage.clear();
  });
});
