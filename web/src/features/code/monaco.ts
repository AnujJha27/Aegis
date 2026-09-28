import * as monaco from "monaco-editor/esm/vs/editor/editor.api";
import { loader } from "@monaco-editor/react";
import editorWorker from "monaco-editor/esm/vs/editor/editor.worker?worker";
import jsonWorker from "monaco-editor/esm/vs/language/json/json.worker?worker";
import cssWorker from "monaco-editor/esm/vs/language/css/css.worker?worker";
import htmlWorker from "monaco-editor/esm/vs/language/html/html.worker?worker";
import tsWorker from "monaco-editor/esm/vs/language/typescript/ts.worker?worker";
import "monaco-editor/esm/vs/basic-languages/cpp/cpp.contribution";
import "monaco-editor/esm/vs/basic-languages/typescript/typescript.contribution";
import "monaco-editor/esm/vs/basic-languages/javascript/javascript.contribution";
import "monaco-editor/esm/vs/language/json/monaco.contribution";
import "monaco-editor/esm/vs/basic-languages/markdown/markdown.contribution";
import "monaco-editor/esm/vs/basic-languages/html/html.contribution";
import "monaco-editor/esm/vs/basic-languages/css/css.contribution";
import "monaco-editor/esm/vs/basic-languages/python/python.contribution";
import "monaco-editor/esm/vs/basic-languages/shell/shell.contribution";
import "monaco-editor/esm/vs/basic-languages/rust/rust.contribution";
import "monaco-editor/esm/vs/basic-languages/go/go.contribution";
import "monaco-editor/esm/vs/basic-languages/yaml/yaml.contribution";
import "monaco-editor/esm/vs/basic-languages/ini/ini.contribution";
import "monaco-editor/esm/vs/basic-languages/solidity/solidity.contribution";

(globalThis as typeof globalThis & { MonacoEnvironment: { getWorker: (_id: string, label: string) => Worker } }).MonacoEnvironment = {
  getWorker: (_id, label) => {
    if (label === "json") return new jsonWorker();
    if (["css", "scss", "less"].includes(label)) return new cssWorker();
    if (["html", "handlebars", "razor"].includes(label)) return new htmlWorker();
    if (["typescript", "javascript"].includes(label)) return new tsWorker();
    return new editorWorker();
  },
};

loader.config({ monaco });

monaco.editor.defineTheme("aegis-muted-dark", {
  base: "vs-dark",
  inherit: true,
  rules: [
    { token: "comment", foreground: "718087", fontStyle: "italic" },
    { token: "string", foreground: "a6b89d" },
    { token: "keyword", foreground: "a6afb9" },
  ],
  colors: {
    "editor.background": "#0a0c0f",
    "editor.foreground": "#c4cbd0",
    "editorLineNumber.foreground": "#59636c",
    "editorLineNumber.activeForeground": "#9aa6ab",
    "editorCursor.foreground": "#82a4aa",
    "editor.selectionBackground": "#29363b",
    "editor.inactiveSelectionBackground": "#1b2529",
    "editor.lineHighlightBackground": "#101417",
    "editorIndentGuide.background1": "#1c2327",
    "editorWidget.background": "#101418",
    "editorWidget.border": "#252b31",
    "editorGutter.background": "#0a0c0f",
    "diffEditor.insertedTextBackground": "#26392d88",
    "diffEditor.removedTextBackground": "#48272b88",
    "diffEditor.insertedLineBackground": "#17231b88",
    "diffEditor.removedLineBackground": "#2a191c88",
    "scrollbarSlider.background": "#53616744",
    "scrollbarSlider.hoverBackground": "#71808766",
    "scrollbarSlider.activeBackground": "#87969c77",
  },
});
