import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, expect, it, vi } from "vitest";
import { api, type FileEntry } from "../../app/api";
import { QuickOpen } from "./QuickOpen";
afterEach(() => { cleanup(); vi.restoreAllMocks(); });
it("queries the native index and ignores stale results", async () => {
  const entry: FileEntry = { path: "new.ts", name: "new.ts", kind: "file", language: "typescript", changed: false, git_status: "", additions: 0, deletions: 0, binary: false };
  let resolve!: (result: { entries: FileEntry[]; version: number; indexing: boolean; elapsed_us: number }) => void;
  const search = vi.spyOn(api, "quickOpen").mockImplementationOnce(() => new Promise((done) => { resolve = done; })).mockResolvedValue({ entries: [entry], version: 2, indexing: false, elapsed_us: 100 });
  const selected = vi.fn(); render(<QuickOpen onSelect={selected} />);
  fireEvent.keyDown(window, { ctrlKey: true, key: "p" });
  fireEvent.change(screen.getByLabelText("Search repository files"), { target: { value: "new" } });
  await screen.findByText("new.ts", { selector: "span" });
  resolve({ entries: [{ ...entry, path: "old.ts", name: "old.ts" }], version: 1, indexing: false, elapsed_us: 1 });
  await waitFor(() => expect(screen.queryByText("old.ts")).toBeNull());
  fireEvent.keyDown(screen.getByLabelText("Search repository files"), { key: "Enter" });
  expect(selected).toHaveBeenCalledWith(entry); expect(search).toHaveBeenCalledWith("new", expect.any(AbortSignal));
});
