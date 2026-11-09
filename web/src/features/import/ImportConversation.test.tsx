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
    render(<ImportConversation repository="/repo" />);
    const input = screen.getByLabelText("Codex JSONL log file") as HTMLInputElement;
    fireEvent.change(input, { target: { files: [fakeFile()] } });
    await screen.findByLabelText("goal");
    const key = profileStorageKey("/repo");
    expect(localStorage.getItem(key)).toBeNull();

    fireEvent.click(screen.getByRole("button", { name: "Retain as working instruction" }));
    const stored = localStorage.getItem(key) ?? "";
    expect(stored).toContain("s1");
    expect(stored).toContain("log.jsonl:2");
    expect(stored).toContain("Do the thing");
    localStorage.clear();
  });
});
