import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, expect, it, vi } from "vitest";
import { VirtualCodeView } from "./VirtualCodeView";
import { VirtualDiffView } from "./VirtualDiffView";
import { diffHunks, diffLayout } from "./diff";
import { indexLines, visibleRange } from "./lines";
afterEach(cleanup);
it("indexes CRLF/trailing newline and bounds windows", () => {
  const lines = indexLines("a\r\nb\n");
  expect(lines.count).toBe(3); expect(lines.line(0)).toBe("a"); expect(lines.line(2)).toBe("");
  expect(indexLines("a\r\nb\n")).toBe(lines);
  expect(visibleRange(22000, 660, 100000)).toEqual({ first: 988, end: 1042 });
});
it("mounts only visible code rows and scrolls a 100k-line file", async () => {
  const view = render(<VirtualCodeView source={Array.from({ length: 100000 }, (_, i) => `line ${i}`).join("\n")} language="plaintext" identity="large" />);
  expect(view.container.querySelectorAll("[data-row]").length).toBeLessThan(100);
  const viewport = screen.getByLabelText("Read-only code");
  fireEvent.scroll(viewport, { target: { scrollTop: 22000 } });
  await waitFor(() => expect(view.container.querySelector('[data-row="1000"]')).toBeTruthy());
  expect(view.container.querySelector('[data-row="0"]')).toBeNull();
  fireEvent.change(screen.getByLabelText("Find in file"), { target: { value: "line 99999" } });
  fireEvent.click(screen.getByText("Next match"));
  await screen.findByText("Line 100000");
});
it("preserves old/new text across insert/delete/modify diff rows", () => {
  const old = "first\nold\nanchor\nlast", next = "first\nnew\nadded\nanchor\nlast";
  const hunks = diffHunks(old, next), layout = diffLayout(hunks, false), rows = Array.from({ length: layout.count }, (_, i) => layout.row(i)), a = old.split("\n"), b = next.split("\n");
  expect(rows.filter((r) => r.old >= 0).map((r) => a[r.old]).join("\n")).toBe(old);
  expect(rows.filter((r) => r.next >= 0).map((r) => b[r.next]).join("\n")).toBe(next);
  const unified = diffLayout(hunks, true);
  expect(Array.from({ length: unified.count }, (_, i) => unified.row(i)).some((r) => r.kind === "delete")).toBe(true);
});
it("virtualizes split and unified diffs", async () => {
  vi.stubGlobal("Worker", undefined);
  const source = Array.from({ length: 10000 }, (_, i) => `line ${i}`).join("\n");
  const view = render(<VirtualDiffView original={source} modified={source.replace("line 400", "changed 400")} language="plaintext" identity="diff" inline={false} />);
  await screen.findByText("1 changed regions");
  expect(view.container.querySelectorAll("[data-row]").length).toBeLessThan(100);
  fireEvent.click(screen.getByText("Next change"));
  await waitFor(() => expect(view.container.querySelector('[data-row="400"]')).toBeTruthy());
  view.rerender(<VirtualDiffView original={source} modified={source.replace("line 400", "changed 400")} language="plaintext" identity="diff" inline />);
  expect(view.container.querySelectorAll("[data-row]").length).toBeLessThan(100);
  vi.unstubAllGlobals();
});

it("compresses huge unchanged and replacement runs instead of allocating a row object per line", () => {
  const text = "\n".repeat(100000);
  const hunks = diffHunks(text, text);
  expect(hunks.length).toBe(1);
  const layout = diffLayout(hunks, false);
  expect(layout.count).toBe(100001); expect(layout.row(99999).kind).toBe("equal");
  const replacement = diffLayout(diffHunks("a\n".repeat(100000), "b\n".repeat(100000)), true);
  expect(replacement.count).toBe(200001);
});
