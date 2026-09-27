import { useEffect, useState, type ReactNode } from "react";
import { api, type FileEntry } from "../../app/api";

type Props = { scope: "changed" | "all"; refreshKey: unknown; selectedPath: string; onSelect: (file: FileEntry) => void };

export function FileTree({ scope, refreshKey, selectedPath, onSelect }: Props) {
  const [children, setChildren] = useState<Record<string, FileEntry[]>>({});
  const [expanded, setExpanded] = useState<Set<string>>(new Set([""]));
  const [loading, setLoading] = useState<Set<string>>(new Set());
  const [error, setError] = useState("");

  async function load(path: string, force = false) {
    if (children[path] && !force) return;
    setLoading((current) => new Set(current).add(path));
    try {
      const listing = await api.files(path, scope);
      setChildren((current) => ({ ...current, [path]: listing.entries }));
      setError("");
    } catch (reason) { setError(reason instanceof Error ? reason.message : "Could not load files"); }
    finally { setLoading((current) => { const next = new Set(current); next.delete(path); return next; }); }
  }

  useEffect(() => {
    setChildren({});
    setExpanded(new Set([""]));
    void load("");
  }, [scope, refreshKey]);

  async function toggle(entry: FileEntry) {
    if (entry.kind !== "directory") { onSelect(entry); return; }
    if (expanded.has(entry.path)) setExpanded((current) => { const next = new Set(current); next.delete(entry.path); return next; });
    else {
      setExpanded((current) => new Set(current).add(entry.path));
      await load(entry.path);
    }
  }

  function rows(parent: string, depth: number): ReactNode {
    return (children[parent] ?? []).map((entry) => <div key={entry.path}>
      <button className={`file-row ${entry.kind === "directory" ? "directory" : ""} ${selectedPath === entry.path ? "selected" : ""}`}
        style={{ paddingLeft: `${10 + depth * 14}px` }} aria-current={selectedPath === entry.path ? "page" : undefined}
        aria-expanded={entry.kind === "directory" ? expanded.has(entry.path) : undefined}
        onClick={() => void toggle(entry)}>
        <span className="file-tree-glyph">{entry.kind === "directory" ? (expanded.has(entry.path) ? "▾" : "▸") : "·"}</span>
        <span className="file-name" title={entry.path}>{entry.name}</span>
        {entry.changed && <span className={`file-status status-${entry.git_status.toLowerCase()}`}>{entry.git_status}</span>}
        {entry.changed && Boolean(entry.additions || entry.deletions) && <small className="file-stats">+{entry.additions} −{entry.deletions}</small>}
      </button>
      {entry.kind === "directory" && expanded.has(entry.path) && <>{loading.has(entry.path) ? <small className="file-tree-loading">Loading…</small> : rows(entry.path, depth + 1)}</>}
    </div>);
  }

  return <div className="file-tree" role="tree" aria-label={scope === "changed" ? "Changed files" : "Repository files"}>
    {loading.has("") && !children[""] && <p className="file-tree-empty">Loading files…</p>}
    {error && <p className="file-tree-error" role="alert">{error}</p>}
    {!loading.has("") && !error && !children[""]?.length && <p className="file-tree-empty">{scope === "changed" ? "No changed files" : "No visible files"}</p>}
    {rows("", 0)}
  </div>;
}
