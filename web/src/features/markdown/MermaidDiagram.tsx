import { useEffect, useId, useMemo, useRef, useState } from "react";
import { instanceSvg } from "./security";
import { renderDiagram } from "./mermaid";
function currentTheme(): "dark" | "default" {
  return document.documentElement.dataset.theme === "light" || document.documentElement.dataset.theme === "default" ? "default" : "dark";
}
export function MermaidDiagram({ source }: { source: string }) {
  const host = useRef<HTMLElement>(null), canvas = useRef<HTMLDivElement>(null);
  const [theme, setTheme] = useState(currentTheme);
  const [visible, setVisible] = useState(typeof IntersectionObserver === "undefined");
  const [svg, setSvg] = useState("");
  const id = useId().replace(/[^a-zA-Z0-9_-]/g, "");
  const renderedSvg = useMemo(() => svg ? instanceSvg(svg, `aegis-${id}`) : "", [svg, id]);
  const [error, setError] = useState("");
  const [scale, setScale] = useState(1);
  const [pan, setPan] = useState({ x: 0, y: 0 });
  const [fullscreen, setFullscreen] = useState(false);
  const [copyMessage, setCopyMessage] = useState("");
  const drag = useRef<{ x: number; y: number; left: number; top: number } | undefined>(undefined);
  useEffect(() => {
    const observer = typeof IntersectionObserver === "undefined" ? undefined : new IntersectionObserver(([entry]) => setVisible(entry.isIntersecting), { rootMargin: "200px" });
    if (host.current) observer?.observe(host.current);
    let timer = 0;
    const update = () => { clearTimeout(timer); timer = window.setTimeout(() => setTheme(currentTheme()), 120); };
    const mutation = new MutationObserver(update);
    mutation.observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme", "class", "style"] });
    const visibility = () => { if (document.visibilityState === "hidden") setVisible(false); else setVisible(typeof IntersectionObserver === "undefined" || Boolean(host.current && host.current.getBoundingClientRect().bottom > -200 && host.current.getBoundingClientRect().top < window.innerHeight + 200)); };
    const escape = (event: KeyboardEvent) => { if (event.key === "Escape") { setFullscreen(false); if (document.fullscreenElement === host.current) void document.exitFullscreen?.(); } };
    const full = () => setFullscreen(document.fullscreenElement === host.current);
    document.addEventListener("visibilitychange", visibility); document.addEventListener("fullscreenchange", full); window.addEventListener("keydown", escape);
    return () => { observer?.disconnect(); mutation.disconnect(); clearTimeout(timer); document.removeEventListener("visibilitychange", visibility); document.removeEventListener("fullscreenchange", full); window.removeEventListener("keydown", escape); };
  }, []);
  useEffect(() => {
    if (!visible || document.visibilityState === "hidden") return;
    const controller = new AbortController();
    setError("");
    void renderDiagram(source, theme, controller.signal).then((result) => { if (!controller.signal.aborted) setSvg(result); })
      .catch((reason: Error) => { if (!controller.signal.aborted) { setSvg(""); setError(reason.message); } });
    return () => controller.abort();
  }, [source, theme, visible]);
  const reset = () => { setScale(1); setPan({ x: 0, y: 0 }); };
  function fit() {
    reset(); // SVG's base dimensions use width:100%, height:auto within the available viewport.
    const svgNode = canvas.current?.querySelector("svg");
    const box = canvas.current?.getBoundingClientRect();
    const view = svgNode?.viewBox?.baseVal;
    if (view?.height && box?.height && box.width) setScale(Math.min(1, box.height / (view.height * box.width / view.width)));
  }
  async function toggleFullscreen() {
    if (fullscreen) { setFullscreen(false); if (document.fullscreenElement === host.current) await document.exitFullscreen?.(); }
    else { setFullscreen(true); try { await host.current?.requestFullscreen?.(); } catch { /* The fixed viewport fallback also supports Escape. */ } }
  }
  async function copySource() {
    try { await navigator.clipboard.writeText(source); setCopyMessage("Source copied"); }
    catch { setCopyMessage("Copy unavailable; select the source below."); }
  }
  return <figure ref={host} className={`mermaid-diagram ${fullscreen ? "diagram-fullscreen" : ""}`} aria-label="Mermaid diagram">
    <div className="diagram-controls"><button onClick={() => setScale((s) => Math.min(8, s * 1.25))} aria-label="Zoom in">+</button><button onClick={() => setScale((s) => Math.max(.1, s / 1.25))} aria-label="Zoom out">−</button><button onClick={fit}>Fit</button><button onClick={reset}>100%</button><button onClick={() => void toggleFullscreen()}>{fullscreen ? "Exit fullscreen" : "Fullscreen"}</button><button onClick={() => void copySource()}>Copy source</button><output>{Math.round(scale * 100)}%</output><span aria-live="polite">{copyMessage}</span></div>
    {error ? <p role="alert">Diagram could not render: {error}</p> : <div ref={canvas} className="diagram-canvas" style={{ touchAction: scale > 1 ? "none" : "auto", cursor: scale > 1 ? "grab" : "default" }}
      onPointerDown={(event) => { if (scale <= 1) return; event.currentTarget.setPointerCapture?.(event.pointerId); drag.current = { x: event.clientX, y: event.clientY, left: pan.x, top: pan.y }; }}
      onPointerMove={(event) => { const start = drag.current; if (start) setPan({ x: start.left + event.clientX - start.x, y: start.top + event.clientY - start.y }); }}
      onPointerUp={() => { drag.current = undefined; }} onPointerCancel={() => { drag.current = undefined; }}>
      {svg ? <div className="diagram-svg" style={{ transform: `translate(${pan.x}px, ${pan.y}px) scale(${scale})` }} dangerouslySetInnerHTML={{ __html: renderedSvg }} /> : <p role="status">{visible ? "Rendering diagram…" : "Diagram loads when visible"}</p>}
    </div>}
    <details open={Boolean(error)}><summary>Mermaid source</summary><pre><code>{source}</code></pre></details>
  </figure>;
}
