import { useMemo, useRef, useState } from "react";
import { indexLines } from "./lines";
import { VirtualRows } from "./VirtualRows";
export function highlight(line: string, language: string) {
  if (language === "plaintext" || language === "markdown") return line;
  const tokens = line.slice(0, 4000).split(/(\/\/.*$|#.*$|"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|\b(?:const|let|var|class|struct|return|if|else|for|while|import|export|from|def|fn|public|private|void|int|bool|true|false|null|async|await|function|namespace|using|include)\b|\b\d+(?:\.\d+)?\b)/g);
  return <>{tokens.map((part, i) => i % 2 ? <span key={i} className={part.startsWith("//") || part.startsWith("#") ? "token-comment" : /^["']/.test(part) ? "token-string" : /^\d/.test(part) ? "token-number" : "token-keyword"}>{part}</span> : part)}{line.slice(4000)}</>;
}
export function VirtualCodeView({ source, language, identity, changedLines }: { source: string; language: string; identity: string; changedLines?: Set<number> }) {
  const lines = useMemo(() => indexLines(source), [source]);
  const [query, setQuery] = useState("");
  const [jump, setJump] = useState(0);
  const [message, setMessage] = useState("");
  const foldedSource = useMemo(() => query ? source.toLocaleLowerCase() : "", [source, Boolean(query)]);
  const find = useRef<HTMLInputElement>(null);
  const cursor = useRef(-1);
  function next() {
    if (!query) return;
    let match = foldedSource.indexOf(query.toLocaleLowerCase(), cursor.current + 1);
    if (match < 0) match = foldedSource.indexOf(query.toLocaleLowerCase());
    if (match < 0) { setMessage("No matches"); return; }
    cursor.current = match;
    let low = 0, high = lines.count;
    while (low + 1 < high) { const middle = (low + high) >>> 1; if (lines.offsets[middle] <= match) low = middle; else high = middle; }
    setJump(low + 1); setMessage(`Line ${low + 1}`);
  }
  return <div className="virtual-code" onKeyDown={(event) => { if ((event.ctrlKey || event.metaKey) && event.key === "f") { event.preventDefault(); find.current?.focus(); } }}>
    <div className="viewer-tools"><input ref={find} aria-label="Find in file" placeholder="Find (Ctrl/Cmd+F)" value={query} onChange={(e) => { setQuery(e.target.value); cursor.current = -1; }} onKeyDown={(e) => { if (e.key === "Enter") next(); }} /><button onClick={next}>Next match</button><input aria-label="Go to line" type="number" min={1} max={lines.count} placeholder="Line" onKeyDown={(e) => { if (e.key === "Enter") setJump(Math.max(1, Math.min(lines.count, Number(e.currentTarget.value) || 1))); }} /><span aria-live="polite">{message}</span></div>
    <VirtualRows count={lines.count} width={lines.maxColumns * 8 + 75} identity={identity} jump={jump} copy={() => source} renderRow={(i) => <><span className={`line-number ${changedLines?.has(i + 1) ? "changed" : ""}`}>{i + 1}</span><code>{highlight(lines.line(i), language)}</code></>} />
  </div>;
}
