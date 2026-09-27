import { lazy, Suspense, useCallback, useEffect, useMemo, useState } from "react";
import { api, type AgentEvent, type FileEntry, type GitChange, type GitStatus, type HandoffContext, type VerificationRun } from "../../app/api";
import { GitPanel } from "../git/GitPanel";
import { VerificationPanel } from "../verification/VerificationPanel";
import { FileTabs } from "./FileTabs";
import { FileTree } from "./FileTree";
import { QuickOpen } from "./QuickOpen";

const FileViewer = lazy(() => import("./FileViewer").then((module) => ({ default: module.FileViewer })));

type Props = { taskPrompt?: string; events: AgentEvent[]; gitChanges: GitChange[]; gitStatus?: GitStatus; onGitChanged: () => Promise<void>; verification?: VerificationRun; verificationRunning: boolean; onVerify: () => void; handoff?: HandoffContext; openFilePath?: string };
type Range = "all" | "staged" | "unstaged";

export function CodeWorkspace(props: Props) {
  const [scope, setScope] = useState<"changed" | "all">("changed");
  const [tabs, setTabs] = useState<FileEntry[]>([]);
  const [selected, setSelected] = useState("");
  const [mode, setMode] = useState<"file" | "diff">("diff");
  const [range, setRange] = useState<Range>("all");
  const [inline, setInline] = useState(false);
  const [stageError, setStageError] = useState("");
  const file = tabs.find((item) => item.path === selected);
  const changedKey = useMemo(() => props.gitChanges.map((item) => `${item.path}:${item.old_path}:${item.index_status}:${item.worktree_status}:${item.additions}:${item.deletions}:${item.binary}`).join("|"), [props.gitChanges]);

  const openFile = useCallback((entry: FileEntry) => {
    setSelected(entry.path);
    setMode(entry.changed ? "diff" : "file");
    setTabs((current) => [entry, ...current.filter((item) => item.path !== entry.path)].slice(0, 6));
  }, []);

  useEffect(() => {
    const path = props.openFilePath;
    if (!path) return;
    const existing = tabs.find((item) => item.path === path);
    const changed = props.gitChanges.find((item) => item.path === path);
    openFile(existing ?? { path, name: path.split("/").at(-1) ?? path, kind: "file", language: "plaintext", size: 0, changed: Boolean(changed), git_status: changed ? changed.index_status.trim() || changed.worktree_status.trim() : "", additions: changed?.additions ?? 0, deletions: changed?.deletions ?? 0, binary: changed?.binary ?? false });
  }, [props.openFilePath]);

  const currentChange = props.gitChanges.find((item) => item.path === selected);
  const staged = Boolean(currentChange && currentChange.index_status !== " " && currentChange.index_status !== "?");
  const unstaged = Boolean(currentChange && (currentChange.worktree_status !== " " || currentChange.index_status === "?"));

  async function stage(path: string, doStage: boolean) {
    setStageError("");
    try { doStage ? await api.stage(path) : await api.unstage(path); await props.onGitChanged(); }
    catch (reason) { setStageError(reason instanceof Error ? reason.message : "Git operation failed"); }
  }

  return <section className="code-workspace">
    <aside className="code-rail">
      <div className="code-rail-heading"><span>{scope === "changed" ? "CHANGED FILES" : "REPOSITORY"}</span><QuickOpen onSelect={openFile} /></div>
      <div className="file-scope" role="group" aria-label="File list scope"><button aria-pressed={scope === "changed"} className={scope === "changed" ? "active" : ""} onClick={() => setScope("changed")}>Changed</button><button aria-pressed={scope === "all"} className={scope === "all" ? "active" : ""} onClick={() => setScope("all")}>All files</button></div>
      <FileTree scope={scope} refreshKey={changedKey} selectedPath={selected} onSelect={openFile} />
      <footer className="code-rail-footer">{props.gitStatus?.current_branch || "Detached HEAD"} · {props.gitChanges.length} changed</footer>
    </aside>

    <section className="code-main" aria-label="Read-only code review">
      <FileTabs files={tabs} selected={selected} onSelect={setSelected} onClose={(path) => { const next = tabs.filter((item) => item.path !== path); setTabs(next); if (selected === path) setSelected(next[0]?.path ?? ""); }} />
      {file ? <>
        <header className="code-toolbar"><div className="code-path"><span>{file.changed ? file.git_status || "M" : "FILE"}</span><code>{file.path}</code></div><div className="code-controls">
          <div className="code-segment" role="group" aria-label="File view"><button aria-pressed={mode === "file"} className={mode === "file" ? "active" : ""} onClick={() => setMode("file")}>File</button><button aria-pressed={mode === "diff"} className={mode === "diff" ? "active" : ""} onClick={() => setMode("diff")}>Diff</button></div>
          {mode === "diff" && <><select aria-label="Diff source" value={range} onChange={(event) => setRange(event.target.value as Range)}><option value="all">All changes</option><option value="staged">Staged</option><option value="unstaged">Unstaged</option></select><div className="code-segment" role="group" aria-label="Diff layout"><button aria-pressed={!inline} className={!inline ? "active" : ""} onClick={() => setInline(false)}>Split</button><button aria-pressed={inline} className={inline ? "active" : ""} onClick={() => setInline(true)}>Inline</button></div></>}
        </div></header>
        <div className="code-editor"><Suspense fallback={<div className="code-state">Loading local code renderer…</div>}><FileViewer key={`${file.path}:${mode}:${range}`} file={file} mode={mode} range={range} inline={inline} /></Suspense></div>
        <footer className="code-status"><span>{file.language || "text"}</span><span>UTF-8</span><span>{mode === "file" ? "WORKTREE" : `${range === "all" ? "HEAD ↔ WORKTREE" : range === "staged" ? "HEAD ↔ INDEX" : "INDEX ↔ WORKTREE"}`}</span><span>READ ONLY</span></footer>
      </> : <div className="code-empty"><span>SELECT A FILE TO INSPECT</span><p>Agent output → changed files → review evidence</p><small>Ctrl/Cmd+P quick open</small></div>}
    </section>

    <aside className="review-sidebar">
      <section className="review-side-section"><span className="subheading">TASK CONTEXT</span><p className="task-context">{props.taskPrompt || "No task selected"}</p><small>Repository changes are not assumed to belong to this task.</small></section>
      {file && <section className="review-side-section"><span className="subheading">FILE</span><code className="side-file-path">{file.path}</code><div className="file-meta"><span>Status</span><strong>{file.git_status || (file.changed ? "Modified" : "Unchanged")}</strong><span>Size</span><strong>{file.size.toLocaleString()} bytes</strong>{file.old_path && <><span>Renamed from</span><strong>{file.old_path}</strong></>}</div>{currentChange && <div className="review-stage-actions">{unstaged && <button onClick={() => void stage(file.path, true)}>Stage file</button>}{staged && <button onClick={() => void stage(file.path, false)}>Unstage file</button>}</div>}{stageError && <p className="file-tree-error" role="alert">{stageError}</p>}</section>}
      {file?.changed && <section className="review-side-section"><span className="subheading">PROVENANCE</span><p className="muted">{props.events.find((event) => event.type === "file.changed" && event.content.trim() === file.path) ? `Changed during ${props.events.find((event) => event.type === "file.changed" && event.content.trim() === file.path)?.agent ?? "agent"} run ${(props.events.find((event) => event.type === "file.changed" && event.content.trim() === file.path)?.runId ?? "").slice(0, 8)}` : "Attribution unknown for this repository change."}</p></section>}
      <section className="review-side-section"><div className="side-section-heading"><span className="subheading">VERIFICATION</span></div><VerificationPanel run={props.verification} running={props.verificationRunning} onVerify={props.onVerify} /></section>
      <details className="review-git-details"><summary>Git operations</summary><GitPanel changes={props.gitChanges} status={props.gitStatus} onChanged={props.onGitChanged} showFiles={false} /></details>
      {props.handoff && <details className="review-git-details"><summary>Handoff context</summary><pre className="review-handoff">{props.handoff.prompt}{"\n\n"}{props.handoff.changed_files.join("\n")}</pre></details>}
    </aside>
  </section>;
}
