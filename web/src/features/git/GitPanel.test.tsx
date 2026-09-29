import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, expect, it, vi } from "vitest";
import type { GitStatus } from "../../app/api";
import { GitPanel } from "./GitPanel";

afterEach(cleanup);
afterEach(() => vi.unstubAllGlobals());

it("merges the selected branch and refreshes status after a merge conflict", async () => {
  const fetch = vi.fn().mockResolvedValue({
    ok: false,
    status: 409,
    json: async () => ({ error: { code: "git_operation_failed", message: "CONFLICT (content): Merge conflict in app.cpp" } }),
  });
  vi.stubGlobal("fetch", fetch);
  const onChanged = vi.fn().mockResolvedValue(undefined);
  const status: GitStatus = {
    repository: { path: "/repo", exists: true },
    files: [], branches: ["main", "feature"], current_branch: "main", clean: true, agent_running: false,
  };
  render(<GitPanel changes={[]} status={status} onChanged={onChanged} />);

  fireEvent.change(screen.getByRole("combobox", { name: "Branch" }), { target: { value: "feature" } });
  fireEvent.click(screen.getByRole("button", { name: "Merge" }));

  await waitFor(() => expect(fetch).toHaveBeenCalledWith("/api/git/merge", expect.objectContaining({ method: "POST", body: JSON.stringify({ branch: "feature" }) })));
  await waitFor(() => expect(onChanged).toHaveBeenCalledTimes(1));
  expect(screen.getByRole("alert").textContent).toContain("Merge conflict");
});
