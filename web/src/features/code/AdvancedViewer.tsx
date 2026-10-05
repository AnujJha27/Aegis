import Editor, { DiffEditor } from "@monaco-editor/react";
import "./monaco";
const options = { readOnly: true, domReadOnly: true, minimap: { enabled: false }, automaticLayout: true, scrollBeyondLastLine: false, wordWrap: "off" as const };
export default function AdvancedViewer({ source, original, language, inline }: { source: string; original?: string; language: string; inline: boolean }) {
  return original === undefined ? <Editor height="100%" value={source} language={language} options={options} theme="aegis-muted-dark" /> : <DiffEditor height="100%" original={original} modified={source} language={language} options={{ ...options, renderSideBySide: !inline, originalEditable: false }} theme="aegis-muted-dark" />;
}
