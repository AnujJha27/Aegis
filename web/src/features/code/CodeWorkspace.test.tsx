import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { api, type FileEntry, type GitChange } from "../../app/api";
import { CodeWorkspace } from "./CodeWorkspace";

vi.mock("./FileViewer", () => ({
  FileViewer: ({ file, mode, range, inline }: { file: FileEntry; mode: string; range: string; inline: boolean }) =>
    <div data-testid="viewer">{mode}:{file.path}:{range}:{inline ? "inline" : "split"}</div>,
}));

vi.mock("../git/GitPanel", () => ({ GitPanel: () => <div>Git operations</div> }));
vi.mock("../verification/VerificationPanel", () => ({ VerificationPanel: () => <div>Verification evidence</div> }));

const file: FileEntry = { path: "src/agent.cpp", name: "agent.cpp", kind: "file", language: "cpp", size: 120, changed: true, git_status: "M", old_path: null, additions: 2, deletions: 1, binary: false };
const change: GitChange = { path: file.path, old_path: null, index_status: "M", worktree_status: "M", additions: 2, deletions: 1, binary: false };

afterEach(() => { cleanup(); vi.restoreAllMocks(); });

describe("read-only review workspace", () => {
  it("loads a changed file, switches source/layout, and stages it from its review context", async () => {
    vi.spyOn(api, "files").mockImplementation(async (path = "") => ({ entries: path ? [file] : [{ ...file, path: "src", name: "src", kind: "directory", language: "", changed: true }], truncated: false }));
    const stage = vi.spyOn(api, "stage").mockResolvedValue({} as never);
    const onGitChanged = vi.fn().mockResolvedValue(undefined);
    render(<CodeWorkspace events={[]} gitChanges={[change]} onGitChanged={onGitChanged} verificationRunning={false} onVerify={() => {}} />);

    fireEvent.click(await screen.findByRole("button", { name: /src/ }));
    fireEvent.click(await screen.findByRole("button", { name: /agent\.cpp/ }));
    expect((await screen.findByTestId("viewer")).textContent).toContain("diff:src/agent.cpp:all:split");

    fireEvent.click(screen.getByRole("button", { name: "File" }));
    expect(screen.getByTestId("viewer").textContent).toContain("file:src/agent.cpp");
    fireEvent.click(screen.getByRole("button", { name: "Diff" }));
    fireEvent.change(screen.getByRole("combobox", { name: "Diff source" }), { target: { value: "staged" } });
    expect(screen.getByTestId("viewer").textContent).toContain("diff:src/agent.cpp:staged:split");
    fireEvent.click(screen.getByRole("button", { name: "Inline" }));
    expect(screen.getByTestId("viewer").textContent).toContain(":inline");
    fireEvent.click(screen.getByRole("button", { name: "Stage file" }));
    await waitFor(() => expect(stage).toHaveBeenCalledWith(file.path));
    expect(onGitChanged).toHaveBeenCalledOnce();
  });
});
