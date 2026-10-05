import { useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { VirtualRows } from "./VirtualRows";
import { api, type FileEntry, type GitChange } from "../../app/api";

type Props = { scope: "changed" | "all"; changes: GitChange[]; refreshKey: unknown; selectedPath: string; onSelect: (file: FileEntry) => void };

function changeStatus(change: GitChange) {
  const statuses = `${change.index_status}${change.worktree_status}`;
  if (statuses.includes("C")) return "C";
  if (change.old_path || statuses.includes("R")) return "R";
  if (statuses.includes("?")) return "?";
  if (statuses.includes("A")) return "A";
  if (statuses.includes("D")) return "D";
  return "M";
}

function languageFor(path: string) {
  const extension = path.slice(path.lastIndexOf(".")).toLowerCase();
  return ({ ".c": "cpp", ".cc": "cpp", ".cpp": "cpp", ".cxx": "cpp", ".h": "cpp", ".hh": "cpp", ".hpp": "cpp", ".ts": "typescript", ".tsx": "typescript", ".js": "javascript", ".jsx": "javascript", ".json": "json", ".md": "markdown", ".mdx": "markdown", ".htm": "html", ".html": "html", ".css": "css", ".scss": "css", ".py": "python", ".sh": "shell", ".rs": "rust", ".go": "go", ".yaml": "yaml", ".yml": "yaml", ".toml": "ini", ".sol": "solidity" } as Record<string, string>)[extension] ?? "plaintext";
}

function changedEntries(changes: GitChange[], parent: string): FileEntry[] {
  const prefix = parent ? `${parent}/` : "";
  const entries = new Map<string, FileEntry>();
  for (const change of changes) {
    if (!change.path.startsWith(prefix)) continue;
    const tail = change.path.slice(prefix.length);
    const slash = tail.indexOf("/");
    const directory = slash !== -1;
    const name = directory ? tail.slice(0, slash) : tail;
    const path = prefix + name;
    if (!name || entries.has(path)) continue;
    entries.set(path, {
      path, name, kind: directory ? "directory" : "file", language: directory ? "" : languageFor(path),
      changed: true, git_status: changeStatus(change), old_path: directory ? null : change.old_path,
      additions: directory ? 0 : change.additions, deletions: directory ? 0 : change.deletions,
      binary: directory ? false : change.binary,
    });
  }
  return [...entries.values()].sort((left, right) => left.name.localeCompare(right.name));
}

export function FileTree({ scope, changes, refreshKey, selectedPath, onSelect }: Props) {
  const [children, setChildren] = useState<Record<string, FileEntry[]>>({});
  const [expanded, setExpanded] = useState<Set<string>>(new Set([""]));
  const [loading, setLoading] = useState<Set<string>>(new Set());
  const [error, setError] = useState("");
  const [nextOffsets, setNextOffsets] = useState<Record<string, number | undefined>>({});
  const indexVersion = useRef(0);
  const generation = useRef(0);
  const previousRefreshKey = useRef(refreshKey);
  const changesByPath = useMemo(() => new Map(changes.map((change) => [change.path, change])), [changes]);
  const changedDirectories = useMemo(() => {
    const directories = new Map<string, GitChange>();
    for (const change of changes) {
      let slash = change.path.indexOf("/");
      while (slash !== -1) {
        const directory = change.path.slice(0, slash);
        if (!directories.has(directory)) directories.set(directory, change);
        slash = change.path.indexOf("/", slash + 1);
      }
    }
    return directories;
  }, [changes]);

  async function load(path: string, force = false, requestGeneration = generation.current, offset = 0) {
    if (children[path] && !force) return;
    setLoading((current) => new Set(current).add(path));
    try {
      const listing = await (offset ? api.files(path, "all", false, false, offset) : api.files(path, "all", false, false));
      if (requestGeneration !== generation.current) return;
      setChildren((current) => {
        const next = { ...current, [path]: offset ? [...(current[path] ?? []), ...listing.entries] : listing.entries };
        // Cache/expand at most 128 directories; older directories reload on demand.
        for (const key of Object.keys(next)) if (Object.keys(next).length > 128 && key && key !== path) delete next[key];
        return next;
      });
      setNextOffsets((current) => {
        const next = { ...current, [path]: listing.next_offset ?? undefined };
        for (const key of Object.keys(next)) if (Object.keys(next).length > 128 && key && key !== path) delete next[key];
        return next;
      });
      if (listing.indexing) window.setTimeout(() => { if (generation.current === requestGeneration) void load(path, true, requestGeneration); }, 200);
      setError("");
    } catch (reason) {
      if (requestGeneration === generation.current) setError(reason instanceof Error ? reason.message : "Could not load files");
    } finally {
      if (requestGeneration === generation.current) setLoading((current) => { const next = new Set(current); next.delete(path); return next; });
    }
  }

  useEffect(() => {
    const requestGeneration = ++generation.current;
    const repositoryChanged = previousRefreshKey.current !== refreshKey;
    previousRefreshKey.current = refreshKey;
    if (repositoryChanged) { setChildren({}); setNextOffsets({}); }
    setExpanded(new Set([""]));
    setLoading(new Set());
    setError("");
    if (scope === "all" && (repositoryChanged || !children[""])) {
      void load("", true, requestGeneration);
    }
    return () => { if (generation.current === requestGeneration) generation.current++; };
  }, [scope]);

  useEffect(() => {
    const changed = (event: Event) => {
      const next = (event as CustomEvent<{ index_version: number }>).detail.index_version;
      if (scope !== "all" || !next || indexVersion.current === next) return;
      indexVersion.current = next;
      const requestGeneration = ++generation.current;
      for (const path of expanded) void load(path, true, requestGeneration);
    };
    const resync = () => {
      indexVersion.current = 0;
      if (scope !== "all") return;
      const requestGeneration = ++generation.current;
      for (const path of expanded) void load(path, true, requestGeneration);
    };
    window.addEventListener("aegis:repository", changed); window.addEventListener("aegis:resync", resync);
    return () => { window.removeEventListener("aegis:repository", changed); window.removeEventListener("aegis:resync", resync); };
  }, [scope, expanded]);

  function entries(parent: string) {
    if (scope === "changed") return changedEntries(changes, parent);
    return (children[parent] ?? []).map((entry) => {
      const change = changesByPath.get(entry.path) ?? changedDirectories.get(entry.path);
      if (!change) return entry;
      const isFile = entry.kind !== "directory";
      return {
        ...entry, changed: true, git_status: changeStatus(change),
        old_path: isFile ? change.old_path : null,
        additions: isFile ? change.additions : 0,
        deletions: isFile ? change.deletions : 0,
        binary: isFile && change.binary,
      };
    });
  }

  async function toggle(entry: FileEntry) {
    if (entry.kind !== "directory") { onSelect(entry); return; }
    if (expanded.has(entry.path)) setExpanded((current) => { const next = new Set(current); next.delete(entry.path); return next; });
    else {
      setExpanded((current) => { const next = new Set(current).add(entry.path); if (next.size > 128) next.delete([...next].find((path) => path !== "" && path !== entry.path)!); return next; });
      if (scope === "all") await load(entry.path);
    }
  }

  const flattened: { entry: FileEntry; depth: number }[] = [];
  function flatten(parent: string, depth: number) {
    for (const entry of entries(parent)) {
      flattened.push({ entry, depth });
      if (entry.kind === "directory" && expanded.has(entry.path)) flatten(entry.path, depth + 1);
    }
    if (nextOffsets[parent] !== undefined) flattened.push({ entry: { path: parent + "\0more", name: "Load more…", kind: "file", language: "", changed: false, git_status: "", additions: 0, deletions: 0, binary: false }, depth });
  }
  flatten("", 0);
  function row(index: number): ReactNode {
    const { entry, depth } = flattened[index];
    if (entry.path.endsWith("\0more")) { const parent = entry.path.slice(0, -5); return <button className="file-row" disabled={loading.has(parent)} onClick={() => void load(parent, true, generation.current, nextOffsets[parent])}>Load more…</button>; }
    return <button className={`file-row ${entry.kind === "directory" ? "directory" : ""} ${selectedPath === entry.path ? "selected" : ""}`}
      style={{ paddingLeft: `${10 + depth * 14}px`, height: 22 }} aria-current={selectedPath === entry.path ? "page" : undefined}
      aria-expanded={entry.kind === "directory" ? expanded.has(entry.path) : undefined} onClick={() => void toggle(entry)}>
      <span className="file-tree-glyph">{entry.kind === "directory" ? (expanded.has(entry.path) ? "▾" : "▸") : "·"}</span>
      <span className="file-name" title={entry.path}>{entry.name}</span>
      {entry.changed && <span className={`file-status status-${entry.git_status.toLowerCase()}`}>{entry.git_status}</span>}
      {entry.changed && Boolean(entry.additions || entry.deletions) && <small className="file-stats">+{entry.additions} −{entry.deletions}</small>}
    </button>;
  }

  return <div className="file-tree" role="group" aria-label={scope === "changed" ? "Changed files" : "Repository files"}>
    {loading.has("") && !children[""] && <p className="file-tree-empty">Loading files…</p>}
    {error && <p className="file-tree-error" role="alert">{error}</p>}
    {!loading.has("") && !error && !entries("").length && <p className="file-tree-empty">{scope === "changed" ? "No changed files" : "No visible files"}</p>}
    <VirtualRows count={flattened.length} width={180} identity={`tree:${scope}`} copy={() => flattened.map((item) => item.entry.path).join("\n")} renderRow={row} />
  </div>;
}
