import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { api, type FileContent, type FileEntry } from "../../app/api";
import { FileViewer } from "./FileViewer";

vi.mock("./monaco", () => ({}));
vi.mock("@monaco-editor/react", () => ({
  default: ({ value }: { value: string }) => <div data-testid="monaco-editor">{value}</div>,
  DiffEditor: () => <div data-testid="monaco-diff" />,
}));

const file: FileEntry = { path: "generated/huge.cpp", name: "huge.cpp", kind: "file", language: "cpp", size: 9 * 1024 * 1024, changed: false, git_status: "", old_path: null, additions: 0, deletions: 0, binary: false };
const capped: FileContent = { source: "worktree", size: file.size ?? 120, exists: true, binary: false, truncated: true, content: "" };

afterEach(() => { cleanup(); vi.restoreAllMocks(); });

describe("read-only file size limits", () => {
  it("reloads the selected file when repository state refreshes", async () => {
    const read = vi.spyOn(api, "fileContent")
      .mockResolvedValueOnce({ ...capped, size: 3, truncated: false, content: "old" })
      .mockResolvedValueOnce({ ...capped, size: 3, truncated: false, content: "new" });
    const view = render(<FileViewer file={file} mode="file" range="all" inline={false} refreshKey={{}} />);
    await screen.findByText("old");

    view.rerender(<FileViewer file={file} mode="file" range="all" inline={false} refreshKey={{}} />);

    await waitFor(() => expect(screen.getByTestId("monaco-editor").textContent).toBe("new"));
    expect(read).toHaveBeenCalledTimes(2);
  });

  it("keeps files beyond the explicit cap in a metadata state instead of rendering an empty editor", async () => {
    const read = vi.spyOn(api, "fileContent").mockResolvedValue(capped);
    render(<FileViewer file={file} mode="file" range="all" inline={false} />);
    fireEvent.click(await screen.findByRole("button", { name: "Load up to 8 MB" }));
    await screen.findByText(/exceeds the 8 MB display limit/i);
    expect(read).toHaveBeenCalledWith(file.path, "worktree", true);
    expect(screen.queryByTestId("monaco-editor")).toBeNull();
  });
});
