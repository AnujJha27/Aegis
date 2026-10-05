import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, expect, it, vi } from "vitest";
import MarkdownPreview from "./MarkdownPreview";
import { MermaidDiagram } from "./MermaidDiagram";
import { diagramCacheStats, renderDiagram } from "./mermaid";
import { sanitizeMarkdown, sanitizeSvg, validateDiagramSource } from "./security";
const mock = vi.hoisted(() => ({ initialize: vi.fn(), render: vi.fn(async (_id: string, source: string) => {
  if (source.includes("INVALID")) throw new Error("Invalid syntax");
  return { svg: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 100"><text>Diagram rendered</text></svg>', bindFunctions: vi.fn() };
}) }));
vi.mock("mermaid", () => ({ default: mock }));
afterEach(() => { cleanup(); vi.clearAllMocks(); });
it("loads Mermaid only for Mermaid fences and isolates errors", async () => {
  const plain = render(<MarkdownPreview source={"# Hello\n\n```typescript\nconst x = 1\n```"} />);
  expect(screen.getByText("Hello")).toBeTruthy(); expect(mock.render).not.toHaveBeenCalled(); plain.unmount();
  render(<MarkdownPreview source={'```mermaid\nINVALID\n```\n\n```mermaid\nflowchart LR\nA --> B\n```'} />);
  await screen.findByText(/Invalid syntax/); await screen.findByText("Diagram rendered");
  expect(screen.getByText("INVALID", { selector: "code" })).toBeTruthy();
  expect(mock.initialize).toHaveBeenCalledWith(expect.objectContaining({ startOnLoad: false, securityLevel: "strict", htmlLabels: false, suppressErrorRendering: true }));
});
it("caches SVGs, keeps zoom independent of rendering, copies source and exits fullscreen", async () => {
  const source = "flowchart LR\nCacheA --> CacheB";
  const clipboard = vi.fn().mockResolvedValue(undefined);
  Object.defineProperty(navigator, "clipboard", { configurable: true, value: { writeText: clipboard } });
  const view = render(<MermaidDiagram source={source} />); await screen.findByText("Diagram rendered");
  const calls = mock.render.mock.calls.length;
  fireEvent.click(screen.getByLabelText("Zoom in")); fireEvent.click(screen.getByText("Fit")); fireEvent.click(screen.getByText("100%", { selector: "button" }));
  expect(mock.render).toHaveBeenCalledTimes(calls);
  fireEvent.click(screen.getByText("Copy source")); await waitFor(() => expect(clipboard).toHaveBeenCalledWith(source));
  fireEvent.click(screen.getByText("Fullscreen")); expect(view.container.querySelector(".diagram-fullscreen")).toBeTruthy();
  fireEvent.keyDown(window, { key: "Escape" }); expect(view.container.querySelector(".diagram-fullscreen")).toBeNull();
  view.unmount(); render(<MermaidDiagram source={source} />); await screen.findByText("Diagram rendered");
  expect(mock.render).toHaveBeenCalledTimes(calls); expect(diagramCacheStats().entries).toBeGreaterThan(0);
});
it("rerenders visible diagrams for theme changes", async () => {
  render(<MermaidDiagram source="flowchart LR\nThemeA --> ThemeB" />); await screen.findByText("Diagram rendered");
  document.documentElement.dataset.theme = "light";
  await waitFor(() => expect(mock.initialize).toHaveBeenCalledWith(expect.objectContaining({ theme: "default" })));
  delete document.documentElement.dataset.theme;
});
it("rejects stale queued renders and malicious directives", async () => {
  const controller = new AbortController(); controller.abort();
  await expect(renderDiagram("flowchart LR\nStaleA --> StaleB", "dark", controller.signal)).rejects.toMatchObject({ name: "AbortError" });
  for (const source of ['%%{init: {"securityLevel":"loose"}}%%\nflowchart LR\nA --> B', '---\nconfig:\n  securityLevel: loose\n---\nflowchart LR\nA --> B', 'flowchart LR\nA --> B\nclick A callback']) expect(() => validateDiagramSource(source)).toThrow();
  expect(sanitizeMarkdown('<script>alert(1)</script><img src="https://example.invalid/pixel"><a href="javascript:alert(1)">bad</a>')).not.toMatch(/script|<img|javascript:/);
  expect(sanitizeSvg('<svg xmlns="http://www.w3.org/2000/svg"><style>path { fill: u/**/rl(https://example.invalid/x) }</style><path style="fill:u\\72l(https://example.invalid/x)"/></svg>')).not.toContain("example.invalid");
  const svg = sanitizeSvg('<svg xmlns="http://www.w3.org/2000/svg" onload="alert(1)"><script>alert(1)</script><foreignObject/><image href="https://example.invalid/x"/><a href="javascript:alert(1)"><text>bad</text></a><path style="fill:url(https://example.invalid/x)"/></svg>');
  expect(svg).not.toMatch(/onload|script|foreignObject|<image|href=|https:/);
});

it("evicts SVG cache entries at the configured bound", async () => {
  for (let i = 0; i < 105; i++) await renderDiagram(`flowchart LR\nBound${i} --> End${i}`, "dark", new AbortController().signal);
  expect(diagramCacheStats().entries).toBeLessThanOrEqual(100);
  expect(diagramCacheStats().bytes).toBeLessThanOrEqual(8 * 1024 * 1024);
});
