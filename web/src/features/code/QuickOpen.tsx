import { useEffect, useMemo, useRef, useState } from "react";
import { api, type FileEntry } from "../../app/api";

export function QuickOpen({ onSelect }: { onSelect: (file: FileEntry) => void }) {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState("");
  const [files, setFiles] = useState<FileEntry[]>([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState("");
  const [active, setActive] = useState(0);
  const [truncated, setTruncated] = useState(false);
  const input = useRef<HTMLInputElement>(null);

  useEffect(() => {
    const keyboard = (event: KeyboardEvent) => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "p") { event.preventDefault(); setOpen(true); }
      if (event.key === "Escape") setOpen(false);
    };
    window.addEventListener("keydown", keyboard);
    return () => window.removeEventListener("keydown", keyboard);
  }, []);

  useEffect(() => {
    if (!open) return;
    setQuery("");
    setError("");
    setActive(0);
    setLoading(true);
    void api.files("", "all", true).then((result) => { setFiles(result.entries); setTruncated(result.truncated); }).catch((reason: Error) => setError(reason.message)).finally(() => setLoading(false));
    requestAnimationFrame(() => input.current?.focus());
  }, [open]);

  const matches = useMemo(() => {
    const needle = query.toLowerCase().trim();
    return files.filter((file) => !needle || file.path.toLowerCase().includes(needle)).slice(0, 60);
  }, [files, query]);

  if (!open) return <button className="quick-open-trigger" onClick={() => setOpen(true)} title="Quick open (Ctrl/Cmd+P)">⌕ <kbd>Ctrl P</kbd></button>;
  return <div className="quick-open-backdrop" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget) setOpen(false); }}>
    <section className="quick-open" role="dialog" aria-modal="true" aria-label="Quick open file">
      <input ref={input} aria-label="Search repository files" value={query} onChange={(event) => { setQuery(event.target.value); setActive(0); }} onKeyDown={(event) => {
        if (event.key === "ArrowDown") { event.preventDefault(); setActive((index) => Math.min(matches.length - 1, index + 1)); }
        if (event.key === "ArrowUp") { event.preventDefault(); setActive((index) => Math.max(0, index - 1)); }
        if (event.key === "Enter" && matches[active]) { onSelect(matches[active]); setOpen(false); }
      }} placeholder="Search repository files…" />
      <div className="quick-open-results">{loading ? <p>Indexing visible files…</p> : error ? <p role="alert">{error}</p> : matches.length ? <>{matches.map((file, index) => <button className={index === active ? "active" : ""} key={file.path} onMouseEnter={() => setActive(index)} onClick={() => { onSelect(file); setOpen(false); }}><span>{file.name}</span><small>{file.path}</small></button>)}{truncated && <p>Showing the first 500 files.</p>}</> : <p>No matching files</p>}</div>
      <footer>↑↓ Navigate · Enter Open · Esc Close</footer>
    </section>
  </div>;
}
