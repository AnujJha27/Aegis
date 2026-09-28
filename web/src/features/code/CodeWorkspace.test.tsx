import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { api, type FileEntry, type GitChange, type GitCommit } from "../../app/api";
import { CodeWorkspace } from "./CodeWorkspace";

vi.mock("./FileViewer", () => ({
  FileViewer: ({ file, mode, range, inline, commitId }: { file: FileEntry; mode: string; range: string; inline: boolean; commitId?: string }) =>
    <div data-testid="viewer">{mode}:{file.path}:{range}:{inline ? "inline" : "split"}:{commitId ?? "working-tree"}</div>,
}));

vi.mock("../git/GitPanel", () => ({ GitPanel: () => <div>Git operations</div> }));
vi.mock("../verification/VerificationPanel", () => ({ VerificationPanel: () => <div>Verification evidence</div> }));

const file: FileEntry = { path: "src/agent.cpp", name: "agent.cpp", kind: "file", language: "cpp", size: 120, changed: true, git_status: "M", old_path: null, additions: 2, deletions: 1, binary: false };
const change: GitChange = { path: file.path, old_path: null, index_status: "M", worktree_status: "M", additions: 2, deletions: 1, binary: false };
const commit: GitCommit = { id: "0123456789abcdef0123456789abcdef01234567", parent_id: "fedcba9876543210fedcba9876543210fedcba98", author: "Aegis test", timestamp: 1_750_000_000, subject: "Update agent lifecycle" };

afterEach(() => { cleanup(); vi.restoreAllMocks(); });

describe("read-only review workspace", () => {
  it("offers commit history as a review scope", () => {
    render(<CodeWorkspace events={[]} gitChanges={[]} onGitChanged={vi.fn().mockResolvedValue(undefined)} verificationRunning={false} onVerify={() => {}} />);
    expect(screen.getByRole("button", { name: "Commits" })).toBeTruthy();
  });

  it("opens a read-only file diff for the selected commit without worktree staging actions", async () => {
    vi.spyOn(api, "commits").mockResolvedValue([commit]);
    vi.spyOn(api, "commitReview").mockResolvedValue({ commit, files: [{ path: "agent.cpp", old_path: null, status: "M" }] });
    render(<CodeWorkspace events={[]} gitChanges={[]} onGitChanged={vi.fn().mockResolvedValue(undefined)} verificationRunning={false} onVerify={() => {}} />);

    fireEvent.click(screen.getByRole("button", { name: "Commits" }));
    fireEvent.change(await screen.findByRole("combobox", { name: "Commit to review" }), { target: { value: commit.id } });
    fireEvent.click(await screen.findByRole("button", { name: /agent\.cpp/ }));

    expect((await screen.findByTestId("viewer")).textContent).toContain(`diff:agent.cpp:all:split:${commit.id}`);
    expect(screen.getByText(`${commit.parent_id!.slice(0, 7)} ↔ ${commit.id.slice(0, 7)}`)).toBeTruthy();
    expect(screen.queryByRole("button", { name: "Stage file" })).toBeNull();
  });

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
