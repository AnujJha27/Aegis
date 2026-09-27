import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { api, type FileContent, type FileEntry } from "../../app/api";
import { FileViewer } from "./FileViewer";

vi.mock("./monaco", () => ({}));
vi.mock("@monaco-editor/react", () => ({
  default: () => <div data-testid="monaco-editor" />,
  DiffEditor: () => <div data-testid="monaco-diff" />,
}));

const file: FileEntry = { path: "generated/huge.cpp", name: "huge.cpp", kind: "file", language: "cpp", size: 9 * 1024 * 1024, changed: false, git_status: "", old_path: null, additions: 0, deletions: 0, binary: false };
const capped: FileContent = { source: "worktree", size: file.size, exists: true, binary: false, truncated: true, content: "" };

afterEach(() => { cleanup(); vi.restoreAllMocks(); });

describe("read-only file size limits", () => {
  it("keeps files beyond the explicit cap in a metadata state instead of rendering an empty editor", async () => {
    const read = vi.spyOn(api, "fileContent").mockResolvedValue(capped);
    render(<FileViewer file={file} mode="file" range="all" inline={false} />);
    fireEvent.click(await screen.findByRole("button", { name: "Load up to 8 MB" }));
    await screen.findByText(/exceeds the 8 MB display limit/i);
    expect(read).toHaveBeenCalledWith(file.path, "worktree", true);
    expect(screen.queryByTestId("monaco-editor")).toBeNull();
  });
});
