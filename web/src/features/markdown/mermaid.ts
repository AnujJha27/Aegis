import { sanitizeSvg, validateDiagramSource } from "./security";
let modulePromise: Promise<typeof import("mermaid")> | undefined;
let serial = Promise.resolve();
let sequence = 0;
const cache = new Map<string, string>();
let cacheBytes = 0;
export const diagramConfig = (theme: "dark" | "default") => ({
  startOnLoad: false, securityLevel: "strict" as const, htmlLabels: false, suppressErrorRendering: true,
  maxTextSize: 50000, maxEdges: 1000, theme,
  flowchart: { htmlLabels: false },
  secure: ["secure", "securityLevel", "startOnLoad", "maxTextSize", "maxEdges", "suppressErrorRendering", "htmlLabels", "flowchart", "dompurifyConfig", "themeCSS"],
});
async function keyFor(source: string, theme: string) {
  // Keep exact keys on hosts without SubtleCrypto; the byte bound includes those keys.
  if (!globalThis.crypto?.subtle) return `${theme}:${source}`;
  const hash = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`${theme}\0${source}`));
  return [...new Uint8Array(hash)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}
export async function renderDiagram(source: string, theme: "dark" | "default", signal: AbortSignal): Promise<string> {
  validateDiagramSource(source);
  const key = await keyFor(source, theme);
  const cancelled = () => { if (signal.aborted) throw new DOMException("Diagram no longer active", "AbortError"); };
  cancelled();
  const cached = cache.get(key);
  if (cached) { cache.delete(key); cache.set(key, cached); return cached; }
  // Mermaid owns mutable global config/IDs: one cooperative queue prevents theme and render races.
  let finish!: () => void;
  const previous = serial;
  serial = new Promise<void>((resolve) => { finish = resolve; });
  await previous;
  try {
    cancelled();
    const reused = cache.get(key);
    if (reused) return reused;
    modulePromise ??= import("mermaid").catch((error) => { modulePromise = undefined; throw error; });
    const mermaid = (await modulePromise).default;
    cancelled();
    mermaid.initialize(diagramConfig(theme));
    const container = document.createElement("div");
    container.setAttribute("aria-hidden", "true");
    container.style.cssText = "position:fixed;left:-100000px;top:0;visibility:hidden;pointer-events:none";
    document.body.append(container);
    let svg: string;
    try {
      const rendered = await mermaid.render(`aegis-diagram-${++sequence}`, source, container);
      svg = sanitizeSvg(rendered.svg); // Deliberately ignore bindFunctions and all callback hooks.
    } finally { container.remove(); }
    cancelled();
    const size = (svg.length + key.length) * 2;
    if (size <= 8 * 1024 * 1024) {
      while (cache.size >= 100 || cacheBytes + size > 8 * 1024 * 1024) {
        const oldest = cache.keys().next().value!;
        cacheBytes -= (cache.get(oldest)!.length + oldest.length) * 2; cache.delete(oldest);
      }
      cache.set(key, svg); cacheBytes += size;
    }
    return svg;
  } finally {
    // Yield between diagrams, including invalid ones, so one page cannot monopolize the main thread.
    await new Promise<void>((resolve) => setTimeout(resolve, 0));
    finish();
  }
}
export function diagramCacheStats() { return { entries: cache.size, bytes: cacheBytes }; }
