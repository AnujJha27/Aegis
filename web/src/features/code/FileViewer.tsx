import { lazy, Suspense, useEffect, useRef, useState } from "react";
import { VirtualCodeView } from "./VirtualCodeView";
import { VirtualDiffView } from "./VirtualDiffView";
const MarkdownPreview = lazy(() => import("../markdown/MarkdownPreview"));
const AdvancedViewer = lazy(() => import("./AdvancedViewer"));
import type { FileComparison, FileContent, FileEntry } from "../../app/api";
import { api } from "../../app/api";

type Props = { file: FileEntry; mode: "file" | "diff"; range: "all" | "staged" | "unstaged"; inline: boolean; commitId?: string; refreshKey?: unknown; startedAt?: number };
export function FileViewer({ file, mode, range, inline, commitId, refreshKey, startedAt }: Props) {
  const [repositoryTick, setRepositoryTick] = useState(0);
  const lastClick = useRef(0);
  const requestStarted = useRef(0);
  const activeRequest = useRef<AbortController | undefined>(undefined);
  useEffect(() => {
    let lastVersion = 0;
    const changed = (event: Event) => {
      const update = (event as CustomEvent<{ version: number; paths?: string[]; invalidate_all?: boolean }>).detail;
      if (update.version <= lastVersion) return;
      lastVersion = update.version;
      if (commitId || (!update.invalidate_all && !update.paths?.includes(file.path))) return;
      activeRequest.current?.abort();
      setRepositoryTick((i) => i + 1);
    };
    const resync = () => { lastVersion = 0; activeRequest.current?.abort(); setRepositoryTick((i) => i + 1); };
    window.addEventListener("aegis:repository", changed); window.addEventListener("aegis:resync", resync);
    return () => { window.removeEventListener("aegis:repository", changed); window.removeEventListener("aegis:resync", resync); };
  }, [file.path, commitId]);
  const [preview, setPreview] = useState(false);
  const [advanced, setAdvanced] = useState(false);
  const [content, setContent] = useState<FileContent>();
  const [comparison, setComparison] = useState<FileComparison>();
  const [loading, setLoading] = useState(false);
  const [largePath, setLargePath] = useState("");
  const loadLarge = largePath === file.path;
  const [error, setError] = useState("");

  useEffect(() => {
    let cancelled = false;
    const controller = new AbortController();
    activeRequest.current = controller;
    const started = startedAt && lastClick.current !== startedAt ? startedAt : performance.now();
    lastClick.current = startedAt ?? 0; requestStarted.current = started;
    setContent(undefined);
    setComparison(undefined);
    setError("");
    setLoading(true);
    const request = mode === "file" && !commitId
      ? api.fileContent(file.path, "worktree", loadLarge, controller.signal)
      : commitId ? api.compareCommit(file.path, commitId, loadLarge, controller.signal)
        : api.compareFiles(file.path, range === "unstaged" ? "index" : "head", range === "staged" ? "index" : "worktree", loadLarge, controller.signal);
    void request.then((result) => {
      if (cancelled || controller.signal.aborted) return;
      requestAnimationFrame(() => { if (!cancelled) window.dispatchEvent(new CustomEvent("aegis:perf", { detail: { name: mode === "file" ? "file-first-rows" : "diff-first-content", ms: performance.now() - started } })); });
      if ("original" in result) setComparison(result);
      else setContent(result);
    }).catch((reason: Error) => { if (!cancelled && !controller.signal.aborted) setError(reason.message); })
      .finally(() => { if (!cancelled && !controller.signal.aborted) setLoading(false); });
    return () => { cancelled = true; controller.abort(); };
  }, [file.path, mode, range, commitId, refreshKey, loadLarge, repositoryTick]);

  const loadFull = () => setLargePath(file.path);
  const choice = <button className="renderer-choice" onClick={() => setAdvanced((old) => !old)}>{advanced ? "Fast viewer" : "Advanced viewer (Monaco)"}</button>;
  function render(source: string, original?: string) {
    return <>{choice}{original === undefined && file.language === "markdown" && <button className="renderer-choice markdown-choice" onClick={() => setPreview((old) => !old)}>{preview ? "Source" : "Markdown preview"}</button>}{preview && original === undefined && file.language === "markdown" ? <Suspense fallback={<div className="code-state">Loading Markdown preview…</div>}><MarkdownPreview source={source} /></Suspense> : advanced ? <Suspense fallback={<div className="code-state">Loading advanced viewer…</div>}><AdvancedViewer source={source} original={original} language={file.language} inline={inline} /></Suspense>
      : original === undefined ? <VirtualCodeView source={source} language={file.language} identity={`${file.path}:${commitId ?? "worktree"}`} />
        : <VirtualDiffView original={original} modified={source} language={file.language} identity={`${file.path}:${commitId ?? range}`} inline={inline} startedAt={requestStarted.current} />}</>;
  }

  if (loading) return <div className="code-state">Loading {file.path}…</div>;
  if (error) return <div className="code-state code-error" role="alert">{error}</div>;
  if (mode === "file") {
    const fileContent = commitId ? comparison?.modified : content;
    if (!fileContent) return <div className="code-state">No file content returned.</div>;
    if (!fileContent.exists) return <div className="code-state">This file is absent from the selected revision.</div>;
    if (fileContent.binary) return <div className="code-state"><strong>Binary file</strong><span>{file.path} · {fileContent.size.toLocaleString()} bytes</span></div>;
    if (fileContent.truncated) return <div className="code-state"><strong>{loadLarge ? "File exceeds the 8 MB display limit" : `Large file · ${fileContent.size.toLocaleString()} bytes`}</strong><span>Content is capped to protect the local daemon.</span>{!loadLarge && <button onClick={() => void loadFull()}>Load up to 8 MB</button>}</div>;
    return render(fileContent.content);
  }

  if (!comparison) return <div className="code-state">No comparison returned.</div>;
  if (comparison.binary) return <div className="code-state"><strong>Binary file change</strong><span>{file.path} · text diff is unavailable.</span></div>;
  if (comparison.truncated) return <div className="code-state"><strong>{loadLarge ? "Diff exceeds the 8 MB display limit" : "Large file · diff not loaded"}</strong><span>Content is capped to protect the local daemon.</span>{!loadLarge && <button onClick={() => void loadFull()}>Load up to 8 MB</button>}</div>;
  const original = comparison.original.exists ? comparison.original.content : "";
  const modified = comparison.modified.exists ? comparison.modified.content : "";
  if (!original && !modified) return <div className="code-state">No textual content in this comparison.</div>;
  return render(modified, original);
}
