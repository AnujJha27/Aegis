import { cleanup, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { FileTabs } from "./FileTabs";

afterEach(cleanup);

describe("open file controls", () => {
  it("exposes file selection as a labeled button group with a named close action", () => {
    render(<FileTabs files={[{ path: "src/main.cpp", name: "main.cpp", kind: "file", language: "cpp", size: 1, changed: false, git_status: "", old_path: null, additions: 0, deletions: 0, binary: false }]} selected="src/main.cpp" onSelect={vi.fn()} onClose={vi.fn()} />);

    expect(screen.getByRole("group", { name: "Open files" })).toBeTruthy();
    expect(screen.getByRole("button", { name: "main.cpp" }).getAttribute("aria-pressed")).toBe("true");
    expect(screen.getByRole("button", { name: "Close main.cpp" })).toBeTruthy();
    expect(screen.queryByRole("tablist")).toBeNull();
  });
});
