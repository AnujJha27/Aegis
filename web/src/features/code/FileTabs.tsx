import type { FileEntry } from "../../app/api";

export function FileTabs({ files, selected, onSelect, onClose }: { files: FileEntry[]; selected: string; onSelect: (path: string) => void; onClose: (path: string) => void }) {
  return <div className="code-tabs" role="tablist" aria-label="Open files">
    {files.map((file) => <div className={`code-tab ${selected === file.path ? "active" : ""}`} key={file.path} role="presentation">
      <button role="tab" aria-selected={selected === file.path} onClick={() => onSelect(file.path)} title={file.path}>{file.name}</button>
      <button className="close-tab" aria-label={`Close ${file.name}`} onClick={() => onClose(file.path)}>×</button>
    </div>)}
    {!files.length && <span className="no-tabs">No files open</span>}
  </div>;
}
