import { act, cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { api, type FileEntry, type GitChange } from "../../app/api";
import { FileTree } from "./FileTree";

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}

const entry = (path: string, kind: FileEntry["kind"] = "file"): FileEntry => ({
  path, name: path.split("/").at(-1)!, kind, language: "plaintext", size: 0,
  changed: false, git_status: "", old_path: null, additions: 0, deletions: 0, binary: false,
});

afterEach(() => { cleanup(); vi.restoreAllMocks(); });

describe("file tree", () => {
  it("does not let an older all-files response replace the current changed-files scope", async () => {
    const all = deferred<{ entries: FileEntry[]; truncated: boolean }>();
    const files = vi.spyOn(api, "files").mockReturnValue(all.promise);
    const changes: GitChange[] = [{ path: "src/agent.cpp", old_path: null, index_status: " ", worktree_status: "M", additions: 2, deletions: 1, binary: false }];
    const props = { refreshKey: "", selectedPath: "", onSelect: vi.fn() };
    const view = render(<FileTree {...props} changes={changes} scope="all" />);
    await waitFor(() => expect(files).toHaveBeenCalledWith("", "all", false, false));

    view.rerender(<FileTree {...props} changes={changes} scope="changed" />);
    expect(screen.getByRole("button", { name: /src/ })).toBeTruthy();

    await act(async () => { all.resolve({ entries: [entry("README.md")], truncated: false }); await all.promise; });
    expect(screen.queryByRole("button", { name: /README\.md/ })).toBeNull();
    fireEvent.click(screen.getByRole("button", { name: /src/ }));
    expect(screen.getByRole("button", { name: /agent\.cpp/ })).toBeTruthy();
    expect(files).toHaveBeenCalledOnce();
  });

  it("loads all-file names without Git status work and overlays current change metadata", async () => {
    const changes: GitChange[] = [{ path: "src/agent.cpp", old_path: null, index_status: " ", worktree_status: "M", additions: 2, deletions: 1, binary: false }];
    const files = vi.spyOn(api, "files").mockImplementation(async (path) => ({
      entries: path ? [entry("src/agent.cpp")] : [entry("src", "directory")], truncated: false,
    }));
    render(<FileTree scope="all" changes={changes} refreshKey="" selectedPath="" onSelect={vi.fn()} />);
    fireEvent.click(await screen.findByRole("button", { name: /src/ }));
    const fileRow = await screen.findByRole("button", { name: /agent\.cpp/ });
    expect(fileRow.textContent).toContain("M");
    expect(files).toHaveBeenNthCalledWith(1, "", "all", false, false);
    expect(files).toHaveBeenNthCalledWith(2, "src", "all", false, false);
  });
});
