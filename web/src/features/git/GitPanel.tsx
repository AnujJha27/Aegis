import { useEffect, useState } from "react";
import { api, type GitChange, type GitStatus } from "../../app/api";

export function GitPanel({ changes, status, onChanged, showFiles = true }: { changes: GitChange[]; status?: GitStatus; onChanged: () => Promise<void>; showFiles?: boolean }) {
  const [message, setMessage] = useState("");
  const [branch, setBranch] = useState(status?.current_branch ?? "");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const staged = changes.filter((file) => file.index_status !== " " && file.index_status !== "?");
  const guarded = !status?.clean || status.agent_running;

  useEffect(() => { if (status?.current_branch) setBranch(status.current_branch); }, [status?.current_branch]);

  async function action(work: () => Promise<unknown>, refreshOnFailure = false) {
    setBusy(true);
    setError("");
    setNotice("");
    try {
      const result = await work();
      if (result && typeof result === "object" && "output" in result && typeof result.output === "string") setNotice(result.output.trim());
      await onChanged();
    }
    catch (reason) {
      const message = reason instanceof Error ? reason.message : "Git operation failed";
      if (refreshOnFailure) {
        try { await onChanged(); }
        catch (refreshError) {
          setError(`${message}; status refresh failed: ${refreshError instanceof Error ? refreshError.message : "unknown error"}`);
          return;
        }
      }
      setError(message);
    }
    finally { setBusy(false); }
  }

  return <section className="git-panel">
    <div className="git-panel-heading"><div><span className="subheading">CHANGES</span><small>{changes.length} files · {staged.length} staged</small></div></div>
    <div className="git-remote-controls">
      <select aria-label="Branch" value={branch} onChange={(event) => setBranch(event.target.value)} disabled={busy}>
        {(status?.branches ?? []).map((name) => <option key={name} value={name}>{name}</option>)}
      </select>
      <button disabled={busy || guarded || !branch || branch === status?.current_branch} onClick={() => void action(() => api.switchBranch(branch))}>Switch</button>
      <button disabled={busy || guarded || !branch || branch === status?.current_branch} title="Merge the selected branch into the current branch" onClick={() => void action(() => api.merge(branch), true)}>Merge</button>
      <button disabled={busy || guarded} onClick={() => void action(api.pull)}>Pull</button>
      <button disabled={busy} onClick={() => void action(api.push)}>Push</button>
    </div>
    {status?.agent_running && <small className="muted">Stop the active agent before switching branches, merging, or pulling.</small>}
    {showFiles && !changes.length && <p className="muted">Working tree clean.</p>}
    {showFiles && <div className="git-files">{changes.map((file) => { const isStaged = file.index_status !== " " && file.index_status !== "?"; const isUnstaged = file.worktree_status !== " " || file.index_status === "?"; return <div className="git-file" key={file.path}><code>{file.path}</code><span className="git-file-status">{file.index_status}{file.worktree_status}</span><div className="git-file-actions">{isUnstaged && <button disabled={busy} onClick={() => void action(() => api.stage(file.path))}>Stage</button>}{isStaged && <button disabled={busy} onClick={() => void action(() => api.unstage(file.path))}>Unstage</button>}</div></div>; })}</div>}
    <form className="git-commit" onSubmit={(event) => { event.preventDefault(); if (message.trim()) void action(async () => { const result = await api.commit(message.trim()); setMessage(""); return result; }); }}><input value={message} onChange={(event) => setMessage(event.target.value)} placeholder="Commit message" aria-label="Commit message" /><button disabled={busy || !staged.length || !message.trim()}>{busy ? "Working…" : "Commit staged"}</button></form>
    {notice && <p className="git-notice">{notice}</p>}{error && <p className="git-error" role="alert">{error}</p>}
  </section>;
}
