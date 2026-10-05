import { useEffect, useMemo, useState } from "react";
import { type DiffHunk, diffHunks, diffLayout } from "./diff";
import { indexLines } from "./lines";
import { highlight, VirtualCodeView } from "./VirtualCodeView";
import { VirtualRows } from "./VirtualRows";
export function VirtualDiffView({ original, modified, language, identity, inline, startedAt }: { original: string; modified: string; language: string; identity: string; inline: boolean; startedAt?: number }) {
  const [aligned, setAligned] = useState<DiffHunk[]>();
  const [error, setError] = useState("");
  const [jump, setJump] = useState(0);
  const a = useMemo(() => indexLines(original), [original]), b = useMemo(() => indexLines(modified), [modified]);
  useEffect(() => {
    const started = startedAt ?? performance.now();
    setAligned(undefined); setError("");
    if (typeof Worker === "undefined") { setAligned(diffHunks(original, modified)); return; }
    const worker = new Worker(new URL("./diff.worker.ts", import.meta.url), { type: "module" });
    worker.onmessage = (event: MessageEvent<{ hunks?: DiffHunk[]; error?: string }>) => { setAligned(event.data.hunks); requestAnimationFrame(() => window.dispatchEvent(new CustomEvent("aegis:perf", { detail: { name: "diff-first-hunk", ms: performance.now() - started } }))); setError(event.data.error ?? ""); worker.terminate(); };
    worker.onerror = () => { setError("Could not prepare diff. Use the advanced viewer."); worker.terminate(); };
    worker.postMessage({ original, modified });
    return () => worker.terminate();
  }, [original, modified]);
  const rows = useMemo(() => diffLayout(aligned ?? [], inline), [aligned, inline]);
  const changes = rows.changes;
  if (error) return <p role="alert">{error}</p>;
  if (!aligned) return <><span role="status">Preparing diff…</span><VirtualCodeView source={modified} language={language} identity={identity} /></>;
  return <div className={`virtual-code virtual-diff ${inline ? "unified" : "split"}`}>
    <div className="viewer-tools"><span>{changes.length} changed regions</span><button onClick={() => setJump((changes.find((i) => i + 1 > jump) ?? changes[0] ?? 0) + 1)}>Next change</button><button onClick={() => setJump(([...changes].reverse().find((i) => i + 1 < jump) ?? changes.at(-1) ?? 0) + 1)}>Previous change</button></div>
    <VirtualRows identity={`${identity}:${inline}`} count={rows.count} width={inline ? Math.max(a.maxColumns, b.maxColumns) * 8 + 100 : (a.maxColumns + b.maxColumns) * 8 + 150} jump={jump} copy={() => modified || original} copyRows={(first, end) => {
      const text: string[] = [];
      for (let i = first; i < Math.min(end, rows.count); i++) {
        const row = rows.row(i);
        if (row.kind === "modify") text.push(`-${a.line(row.old)}`, `+${b.line(row.next)}`);
        else text.push(`${row.kind === "add" ? "+" : row.kind === "delete" ? "-" : " "}${row.next >= 0 ? b.line(row.next) : a.line(row.old)}`);
      }
      return text.join("\n");
    }} renderRow={(i) => {
      const row = rows.row(i);
      return inline ? <div className={`diff-cell ${row.kind}`}><span className="line-number">{row.old >= 0 ? row.old + 1 : ""}</span><span className="line-number">{row.next >= 0 ? row.next + 1 : ""}</span><span>{row.kind === "add" ? "+" : row.kind === "delete" ? "−" : " "}</span><code>{highlight(row.next >= 0 ? b.line(row.next) : a.line(row.old), language)}</code></div> : <><div style={{ minWidth: a.maxColumns * 8 + 75 }} className={`diff-cell ${row.kind === "equal" ? "equal" : "delete"}`}><span className="line-number">{row.old >= 0 ? row.old + 1 : ""}</span><code>{row.old >= 0 ? highlight(a.line(row.old), language) : ""}</code></div><div style={{ minWidth: b.maxColumns * 8 + 75 }} className={`diff-cell ${row.kind === "equal" ? "equal" : "add"}`}><span className="line-number">{row.next >= 0 ? row.next + 1 : ""}</span><code>{row.next >= 0 ? highlight(b.line(row.next), language) : ""}</code></div></>;
    }} />
  </div>;
}
