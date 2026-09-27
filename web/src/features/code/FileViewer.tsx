import { useEffect, useState } from "react";
import Editor, { DiffEditor } from "@monaco-editor/react";
import "./monaco";
import type { FileComparison, FileContent, FileEntry } from "../../app/api";
import { api } from "../../app/api";

type Props = { file: FileEntry; mode: "file" | "diff"; range: "all" | "staged" | "unstaged"; inline: boolean };
const editorOptions = { readOnly: true, domReadOnly: true, contextmenu: false, minimap: { enabled: false }, scrollBeyondLastLine: false, automaticLayout: true, wordWrap: "off" as const, glyphMargin: false, folding: false, lineNumbersMinChars: 3, renderLineHighlight: "none" as const, quickSuggestions: false, suggestOnTriggerCharacters: false, parameterHints: { enabled: false }, codeLens: false };
const language = (name: string) => name || "plaintext";

export function FileViewer({ file, mode, range, inline }: Props) {
  const [content, setContent] = useState<FileContent>();
  const [comparison, setComparison] = useState<FileComparison>();
  const [loading, setLoading] = useState(false);
  const [loadLarge, setLoadLarge] = useState(false);
  const [error, setError] = useState("");

  useEffect(() => {
    let cancelled = false;
    setContent(undefined);
    setComparison(undefined);
    setError("");
    setLoadLarge(false);
    setLoading(true);
    const request = mode === "file"
      ? api.fileContent(file.path)
      : api.compareFiles(file.path, range === "unstaged" ? "index" : "head", range === "staged" ? "index" : "worktree");
    void request.then((result) => {
      if (cancelled) return;
      if ("original" in result) setComparison(result);
      else setContent(result);
    }).catch((reason: Error) => { if (!cancelled) setError(reason.message); })
      .finally(() => { if (!cancelled) setLoading(false); });
    return () => { cancelled = true; };
  }, [file.path, mode, range]);

  async function loadFull() {
    setLoading(true);
    setError("");
    try {
      if (mode === "file") setContent(await api.fileContent(file.path, "worktree", true));
      else setComparison(await api.compareFiles(file.path, range === "unstaged" ? "index" : "head", range === "staged" ? "index" : "worktree", true));
      setLoadLarge(true);
    } catch (reason) { setError(reason instanceof Error ? reason.message : "Could not load file"); }
    finally { setLoading(false); }
  }

  if (loading) return <div className="code-state">Loading {file.path}…</div>;
  if (error) return <div className="code-state code-error" role="alert">{error}</div>;
  if (mode === "file") {
    if (!content) return <div className="code-state">No file content returned.</div>;
    if (!content.exists) return <div className="code-state">This file is deleted from the working tree. Switch to Diff to inspect the removal.</div>;
    if (content.binary) return <div className="code-state"><strong>Binary file</strong><span>{file.path} · {content.size.toLocaleString()} bytes</span></div>;
    if (content.truncated && !loadLarge) return <div className="code-state"><strong>Large file · {content.size.toLocaleString()} bytes</strong><span>Content is capped to protect the local daemon.</span><button onClick={() => void loadFull()}>Load up to 8 MB</button></div>;
    return <Editor height="100%" language={language(file.language)} value={content.content} options={editorOptions} theme="vs-dark" />;
  }

  if (!comparison) return <div className="code-state">No comparison returned.</div>;
  if (comparison.binary) return <div className="code-state"><strong>Binary file change</strong><span>{file.path} · text diff is unavailable.</span></div>;
  if (comparison.truncated && !loadLarge) return <div className="code-state"><strong>Large file · diff not loaded</strong><span>Content is capped to protect the local daemon.</span><button onClick={() => void loadFull()}>Load up to 8 MB</button></div>;
  const original = comparison.original.exists ? comparison.original.content : "";
  const modified = comparison.modified.exists ? comparison.modified.content : "";
  if (!original && !modified) return <div className="code-state">No textual content in this comparison.</div>;
  return <DiffEditor height="100%" language={language(file.language)} original={original} modified={modified}
    options={{ ...editorOptions, renderSideBySide: !inline, originalEditable: false, diffWordWrap: "off", ignoreTrimWhitespace: false }} theme="vs-dark" />;
}
